package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

const defaultMailDir = "voicemail"

// MailEntry 一条语音留言的元数据。音频以原始 IMA ADPCM 帧（240 字节/30ms）落盘，
// 设备端不保存音频，只保存少量元数据。
type MailEntry struct {
	ID         string `json:"id"`
	From       string `json:"from"`
	To         string `json:"to"`
	TS         int64  `json:"ts"`
	DurationMS int    `json:"duration_ms"`
	Size       int    `json:"size"`
	Status     string `json:"status"` // new / read
	File       string `json:"file"`
}

type mailStore struct {
	mu   sync.Mutex
	dir  string
	data map[string][]MailEntry // key: 会话目录，如 A2B
}

var store = &mailStore{dir: defaultMailDir, data: map[string][]MailEntry{}}

func mailFolder(from, to string) string {
	return from + "2" + to
}

func validDeviceID(id string) bool {
	if len(id) == 0 || len(id) > 32 {
		return false
	}
	for _, ch := range id {
		if !(ch >= 'a' && ch <= 'z' || ch >= 'A' && ch <= 'Z' ||
			ch >= '0' && ch <= '9' || ch == '_' || ch == '-') {
			return false
		}
	}
	return true
}

func (s *mailStore) load() {
	s.mu.Lock()
	defer s.mu.Unlock()
	entries, err := os.ReadDir(s.dir)
	if err != nil {
		return
	}
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		folder := e.Name()
		raw, err := os.ReadFile(filepath.Join(s.dir, folder, "index.json"))
		if err != nil {
			continue
		}
		var idx struct {
			Messages []MailEntry `json:"messages"`
		}
		if json.Unmarshal(raw, &idx) == nil {
			s.data[folder] = idx.Messages
		}
	}
}

func (s *mailStore) persist(folder string) {
	dir := filepath.Join(s.dir, folder)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return
	}
	raw, err := json.MarshalIndent(struct {
		Messages []MailEntry `json:"messages"`
	}{s.data[folder]}, "", "  ")
	if err != nil {
		return
	}
	tmp := filepath.Join(dir, "index.json.tmp")
	if os.WriteFile(tmp, raw, 0o644) == nil {
		_ = os.Rename(tmp, filepath.Join(dir, "index.json"))
	}
}

// listFor 返回收件人为 to 的全部留言（按时间升序）。
func (s *mailStore) listFor(to string) []MailEntry {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []MailEntry
	for folder, msgs := range s.data {
		if strings.HasSuffix(folder, "2"+to) {
			out = append(out, msgs...)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].TS < out[j].TS })
	return out
}

func (s *mailStore) unreadCount(to string) int {
	n := 0
	for _, m := range s.listFor(to) {
		if m.Status == "new" {
			n++
		}
	}
	return n
}

func (s *mailStore) createUpload(from, to string) (folder, id, path string, err error) {
	folder = mailFolder(from, to)
	id = fmt.Sprintf("%d_%s", time.Now().UnixMilli(), from)
	dir := filepath.Join(s.dir, folder)
	if err = os.MkdirAll(dir, 0o755); err != nil {
		return "", "", "", err
	}
	path = filepath.Join(dir, "msg_"+id+".bin")
	return folder, id, path, nil
}

func (s *mailStore) finishUpload(folder, id, from, to, path string, size int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	entry := MailEntry{
		ID:         id,
		From:       from,
		To:         to,
		TS:         time.Now().UnixMilli(),
		DurationMS: size / 240 * 30,
		Size:       size,
		Status:     "new",
		File:       filepath.Base(path),
	}
	s.data[folder] = append(s.data[folder], entry)
	s.persist(folder)
}

func (s *mailStore) markRead(folder, id string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for i := range s.data[folder] {
		if s.data[folder][i].ID == id {
			s.data[folder][i].Status = "read"
			s.persist(folder)
			return true
		}
	}
	return false
}

func (s *mailStore) remove(folder, id string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	msgs := s.data[folder]
	for i, m := range msgs {
		if m.ID == id {
			_ = os.Remove(filepath.Join(s.dir, folder, m.File))
			s.data[folder] = append(msgs[:i], msgs[i+1:]...)
			s.persist(folder)
			return true
		}
	}
	return false
}

func (s *mailStore) locate(id string) (folder string, entry MailEntry, ok bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for folder, msgs := range s.data {
		for _, m := range msgs {
			if m.ID == id {
				return folder, m, true
			}
		}
	}
	return "", MailEntry{}, false
}
