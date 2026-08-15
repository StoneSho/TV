package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true },
}

const (
	writeWait    = 3 * time.Second  // 单次写超时
	pongWait     = 30 * time.Second // 超过该时间没收到 pong 视为死连接
	pingPeriod   = 20 * time.Second
	outQueueSize = 64 // 每连接写队列深度，满则丢新包（语音可容忍）
)

type wsMessage struct {
	mt   int
	data []byte
}

type Client struct {
	DeviceID     string
	Conn         *websocket.Conn
	send         chan wsMessage
	quit         chan struct{}
	packetCount  int
	controlCount int
	mailUpload   *mailUploadState
	mailStream   *mailStreamState
}

type mailUploadState struct {
	folder, id, path string
	from, to         string
	file             *os.File
	size             int
}

type mailStreamState struct {
	folder, id string
}

var (
	clients = make(map[string]*Client)
	mu      sync.RWMutex
)

// 单连接唯一写协程：所有发往该连接的数据都从这里串行写出，
// 发送方（对端读协程）只入队、不阻塞。
func (c *Client) writer() {
	ticker := time.NewTicker(pingPeriod)
	defer ticker.Stop()

	for {
		select {
		case msg := <-c.send:
			c.Conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.Conn.WriteMessage(msg.mt, msg.data); err != nil {
				return
			}
		case <-ticker.C:
			c.Conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.Conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				return
			}
		case <-c.quit:
			return
		}
	}
}

func (c *Client) sendControl(v interface{}) {
	b, err := json.Marshal(v)
	if err != nil {
		return
	}
	select {
	case c.send <- wsMessage{mt: websocket.TextMessage, data: b}:
	default:
		log.Printf("%s control queue full, dropped", c.DeviceID)
	}
}

func streamMail(c *Client, folder, id, path string, duration int) {
	f, err := os.Open(path)
	if err != nil {
		return
	}
	defer f.Close()
	buf := make([]byte, 240)
	for {
		n, readErr := f.Read(buf)
		if n > 0 {
			pkt := make([]byte, n)
			copy(pkt, buf[:n])
			select {
			case c.send <- wsMessage{mt: websocket.BinaryMessage, data: pkt}:
			case <-c.quit:
				return
			}
		}
		if readErr != nil {
			break
		}
	}
	c.sendControl(map[string]interface{}{"type": "mail_get_end", "id": id})
	mu.Lock()
	if c.mailStream != nil && c.mailStream.id == id {
		c.mailStream = nil
	}
	mu.Unlock()
}

func handleVoice(w http.ResponseWriter, r *http.Request) {
	deviceID := r.URL.Query().Get("id")
	peerID := r.URL.Query().Get("peer")

	if deviceID == "" || peerID == "" {
		http.Error(w, "missing id or peer", 400)
		return
	}

	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Println("upgrade failed:", err)
		return
	}

	// 小包实时语音：关闭 Nagle，避免 30ms 帧被合并造成额外延迟
	if tcp, ok := conn.UnderlyingConn().(*net.TCPConn); ok {
		_ = tcp.SetNoDelay(true)
	}

	client := &Client{
		DeviceID: deviceID,
		Conn:     conn,
		send:     make(chan wsMessage, outQueueSize),
		quit:     make(chan struct{}),
	}

	// 同 ID 重复连接（重连/重启）时，踢掉旧连接，避免僵尸占位
	mu.Lock()
	if old, ok := clients[deviceID]; ok {
		_ = old.Conn.Close()
	}
	clients[deviceID] = client
	mu.Unlock()

	log.Printf("%s connected, peer: %s", deviceID, peerID)

	go client.writer()
	client.sendControl(map[string]interface{}{
		"type":  "mail_notify",
		"count": store.unreadCount(deviceID),
	})

	conn.SetReadLimit(1024)
	conn.SetReadDeadline(time.Now().Add(pongWait))
	conn.SetPongHandler(func(string) error {
		return conn.SetReadDeadline(time.Now().Add(pongWait))
	})

	defer func() {
		if client.mailUpload != nil {
			_ = client.mailUpload.file.Close()
			_ = os.Remove(client.mailUpload.path)
		}
		close(client.quit)
		_ = conn.Close()
		mu.Lock()
		if clients[deviceID] == client {
			delete(clients, deviceID)
		}
		mu.Unlock()
		log.Printf("%s disconnected", deviceID)
	}()

	for {
		messageType, data, err := conn.ReadMessage()
		if err != nil {
			if !websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway) {
				log.Println("read error:", err)
			}
			break
		}
		// 文本帧：心跳 + 语音信箱控制消息
		if messageType != websocket.BinaryMessage {
			client.controlCount++
			if client.controlCount <= 3 || client.controlCount%50 == 0 {
				log.Printf("%s heartbeat/control: %d bytes (type=%d, #%d)",
					deviceID, len(data), messageType, client.controlCount)
			}
			if len(data) > 0 && string(data) != "ping" {
				handleMailControl(client, deviceID, data)
			}
			continue
		}

		// 语音信箱上传：二进制帧直接落盘，不转发给对端
		if client.mailUpload != nil {
			_, _ = client.mailUpload.file.Write(data)
			client.mailUpload.size += len(data)
			continue
		}

		client.packetCount++
		shouldLog := client.packetCount <= 3 || client.packetCount%100 == 0

		mu.RLock()
		peer, ok := clients[peerID]
		mu.RUnlock()

		if !ok {
			if shouldLog {
				log.Printf("%s -> %s: peer offline, %d bytes dropped (packet #%d)",
					deviceID, peerID, len(data), client.packetCount)
			}
			continue
		}

		// gorilla 的 ReadMessage 会复用读缓冲，入队前必须拷贝，
		// 否则下一帧会覆盖上一帧内容。
		packet := make([]byte, len(data))
		copy(packet, data)

		// 非阻塞入队；队列满说明对方网络跟不上，直接丢新包（实时语音可容忍）
		select {
		case peer.send <- wsMessage{mt: messageType, data: packet}:
			if shouldLog {
				log.Printf("%s -> %s: %d bytes (packet #%d)", deviceID, peerID, len(data), client.packetCount)
			}
		default:
			if shouldLog {
				log.Printf("%s -> %s: peer queue full, %d bytes dropped", deviceID, peerID, len(data))
			}
		}
	}
}

func handleMailControl(c *Client, deviceID string, data []byte) {
	var msg map[string]interface{}
	if err := json.Unmarshal(data, &msg); err != nil {
		return
	}
	typ, _ := msg["type"].(string)
	switch typ {
	case "mail_send_start":
		if c.mailUpload != nil {
			return
		}
		from, _ := msg["from"].(string)
		to, _ := msg["to"].(string)
		if !validDeviceID(from) || !validDeviceID(to) {
			return
		}
		folder, id, path, err := store.createUpload(from, to)
		if err != nil {
			log.Printf("mail create failed: %v", err)
			return
		}
		f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
		if err != nil {
			return
		}
		c.mailUpload = &mailUploadState{
			folder: folder, id: id, path: path, from: from, to: to, file: f,
		}
		c.sendControl(map[string]interface{}{"type": "mail_send_ack", "id": id, "folder": folder})
		log.Printf("%s -> %s: mailbox upload start (%s)", from, to, id)

	case "mail_send_end":
		up := c.mailUpload
		if up == nil {
			return
		}
		_ = up.file.Close()
		store.finishUpload(up.folder, up.id, up.from, up.to, up.path, up.size)
		c.mailUpload = nil
		c.sendControl(map[string]interface{}{"type": "mail_send_done", "id": up.id})
		log.Printf("%s -> %s: mailbox saved (%d bytes)", up.from, up.to, up.size)

	case "mail_send_abort":
		up := c.mailUpload
		if up == nil {
			return
		}
		_ = up.file.Close()
		_ = os.Remove(up.path)
		c.mailUpload = nil

	case "mail_list":
		items := store.listFor(deviceID)
		type item struct {
			ID         string `json:"id"`
			From       string `json:"from"`
			TS         int64  `json:"ts"`
			DurationMS int    `json:"duration_ms"`
			Status     string `json:"status"`
		}
		out := make([]item, 0, len(items))
		for _, m := range items {
			out = append(out, item{m.ID, m.From, m.TS, m.DurationMS, m.Status})
		}
		c.sendControl(map[string]interface{}{"type": "mail_list", "items": out})

	case "mail_get":
		id, _ := msg["id"].(string)
		if id == "" {
			return
		}
		mu.Lock()
		streaming := c.mailStream != nil
		mu.Unlock()
		if streaming {
			return
		}
		folder, entry, ok := store.locate(id)
		if !ok {
			return
		}
		mu.Lock()
		c.mailStream = &mailStreamState{folder: folder, id: id}
		mu.Unlock()
		c.sendControl(map[string]interface{}{
			"type": "mail_get_start", "id": id, "duration": entry.DurationMS,
		})
		go streamMail(c, folder, id, filepath.Join(store.dir, folder, entry.File), entry.DurationMS)

	case "mail_read":
		id, _ := msg["id"].(string)
		if id == "" {
			return
		}
		folder, _, ok := store.locate(id)
		if ok {
			store.markRead(folder, id)
		}

	case "mail_delete":
		id, _ := msg["id"].(string)
		if id == "" {
			return
		}
		folder, _, ok := store.locate(id)
		if ok {
			store.remove(folder, id)
		}
	}
}

// GET /status：返回当前在线设备列表
func handleStatus(w http.ResponseWriter, r *http.Request) {
	mu.RLock()
	ids := make([]string, 0, len(clients))
	for id := range clients {
		ids = append(ids, id)
	}
	mu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintf(w, `{"clients":%s}`, mustJSON(ids))
}

func mustJSON(v interface{}) string {
	b, err := json.Marshal(v)
	if err != nil {
		return "[]"
	}
	return string(b)
}

func main() {
	if dir := os.Getenv("MAIL_DIR"); dir != "" {
		store.dir = dir
	}
	store.load()
	log.Printf("mailbox dir: %s", store.dir)

	port := os.Getenv("PORT")
	if port == "" {
		port = "9002"
	}
	http.HandleFunc("/voice", handleVoice)
	http.HandleFunc("/status", handleStatus)
	log.Printf("LoveTV Voice Server starting on :%s...", port)
	log.Fatal(http.ListenAndServe(":"+port, nil))
}
