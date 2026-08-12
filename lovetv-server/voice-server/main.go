package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
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
	DeviceID    string
	Conn        *websocket.Conn
	send        chan wsMessage
	quit        chan struct{}
	packetCount int
	controlCount int
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

	conn.SetReadLimit(1024)
	conn.SetReadDeadline(time.Now().Add(pongWait))
	conn.SetPongHandler(func(string) error {
		return conn.SetReadDeadline(time.Now().Add(pongWait))
	})

	defer func() {
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
		// 只转发二进制语音帧；文本帧是设备心跳/控制消息，服务器侧消化，
		// 避免把 ping 之类的控制包转发给对端造成干扰。
		if messageType != websocket.BinaryMessage {
			client.controlCount++
			if client.controlCount <= 3 || client.controlCount%50 == 0 {
				log.Printf("%s heartbeat/control: %d bytes (type=%d, #%d)",
					deviceID, len(data), messageType, client.controlCount)
			}
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
	port := os.Getenv("PORT")
	if port == "" {
		port = "9002"
	}
	http.HandleFunc("/voice", handleVoice)
	http.HandleFunc("/status", handleStatus)
	log.Printf("LoveTV Voice Server starting on :%s...", port)
	log.Fatal(http.ListenAndServe(":"+port, nil))
}
