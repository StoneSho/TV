#!/usr/bin/env python3
"""LoveTV MQTT 连通性测试 demo

用法（在 lovetv-server 目录下运行）：
  python mqtt_demo.py                 # 自测：订阅 + 发布 + 回环验证
  python mqtt_demo.py --listen        # 监听模式：打印收到的所有消息
  python mqtt_demo.py --send hello    # 发送模式：发一条消息
  python mqtt_demo.py --ws            # 走 WebSocket（默认端口 9001）
  python mqtt_demo.py -t lovetv/a/text --send "imu"

连接参数默认从 .env 读取，也可用命令行参数覆盖。
"""

import argparse
import os
import random
import sys
import time

import paho.mqtt.client as mqtt


def load_env(path=".env"):
    """极简 .env 读取，避免引入额外依赖。"""
    env = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip()
    return env


def build_client(host, port, user, password, client_id, use_ws=False):
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id,
                         transport="websockets" if use_ws else "tcp")
    if user:
        client.username_pw_set(user, password)
    if use_ws:
        # Mosquitto 要求 WebSocket 握手携带 mqtt 子协议
        client.ws_set_options(path="/mqtt", headers={"Sec-WebSocket-Protocol": "mqtt"})
    client.reconnect_delay_set(min_delay=1, max_delay=5)
    return client


def run_selftest(args, env):
    topic = args.topic or f"lovetv/demo/{random.randint(1000, 9999)}"
    count = args.count
    received = []
    errors = []
    times = []

    def on_connect(client, userdata, flags, reason_code, properties=None):
        if reason_code == 0:
            print(f"[OK] 已连接 {args.host}:{args.port}")
            client.subscribe(topic, qos=1)
            print(f"[OK] 已订阅 {topic}")
            for i in range(1, count + 1):
                client.publish(topic, f"demo-{i}", qos=1)
            print(f"[OK] 已发送 {count} 条消息")
        else:
            errors.append(f"连接失败: {reason_code}")
            client.disconnect()

    def on_message(client, userdata, msg):
        received.append(msg.payload.decode())
        times.append(time.time())
        if len(received) >= count:
            client.disconnect()

    client = build_client(args.host, args.port, args.user, args.password,
                          f"lovetv-demo-{random.randint(1000, 9999)}", args.ws)
    client.on_connect = on_connect
    client.on_message = on_message

    start = time.time()
    try:
        client.connect(args.host, args.port, keepalive=30)
        client.loop_start()
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            if errors or len(received) >= count:
                break
            time.sleep(0.1)
        client.loop_stop()
    except Exception as exc:
        print(f"[FAIL] 无法连接: {exc}")
        return 1

    if errors:
        print(f"[FAIL] {errors[0]}")
        return 1

    if len(received) != count or received != [f"demo-{i}" for i in range(1, count + 1)]:
        print(f"[FAIL] 回环验证不一致: 收到 {received}")
        return 1

    elapsed = time.time() - start
    print(f"[OK] 收到全部 {count} 条消息，顺序正确")
    print(f"[OK] 往返耗时总计 {elapsed:.2f}s（含订阅与发布），单条约 {elapsed / count * 1000:.0f}ms")
    print("[PASS] MQTT 服务正常 ✅")
    return 0


def run_listen(args, env):
    topic = args.topic or "lovetv/#"

    def on_connect(client, userdata, flags, reason_code, properties=None):
        if reason_code == 0:
            print(f"[OK] 已连接，监听 {topic}（Ctrl+C 退出）")
            client.subscribe(topic, qos=1)
        else:
            print(f"[FAIL] 连接失败: {reason_code}")
            sys.exit(1)

    def on_message(client, userdata, msg):
        print(f"[MSG] {msg.topic}: {msg.payload.decode(errors='replace')}")

    client = build_client(args.host, args.port, args.user, args.password,
                          f"lovetv-listen-{random.randint(1000, 9999)}", args.ws)
    client.on_connect = on_connect
    client.on_message = on_message
    try:
        client.connect(args.host, args.port, keepalive=30)
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[OK] 已退出监听")
    except Exception as exc:
        print(f"[FAIL] 无法连接: {exc}")
        return 1
    return 0


def run_send(args, env):
    topic = args.topic or f"lovetv/demo/{random.randint(1000, 9999)}"

    def on_connect(client, userdata, flags, reason_code, properties=None):
        if reason_code == 0:
            result = client.publish(topic, args.send, qos=1)
            if result.rc == mqtt.MQTT_ERR_SUCCESS:
                print(f"[OK] 已发送到 {topic}: {args.send}")
            else:
                print(f"[FAIL] 发布失败 rc={result.rc}")
            client.disconnect()
        else:
            print(f"[FAIL] 连接失败: {reason_code}")
            sys.exit(1)

    client = build_client(args.host, args.port, args.user, args.password,
                          f"lovetv-send-{random.randint(1000, 9999)}", args.ws)
    client.on_connect = on_connect
    try:
        client.connect(args.host, args.port, keepalive=30)
        client.loop_start()
        time.sleep(2)
        client.loop_stop()
    except Exception as exc:
        print(f"[FAIL] 无法连接: {exc}")
        return 1
    return 0


def main():
    env = load_env()
    parser = argparse.ArgumentParser(description="LoveTV MQTT 测试 demo")
    parser.add_argument("--host", default=env.get("MQTT_HOST", "117.72.44.85"))
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--user", default=env.get("MQTT_USER", "lovetv"))
    parser.add_argument("--password", default=env.get("MQTT_PASSWORD", ""))
    parser.add_argument("--ws", action="store_true", help="使用 WebSocket 连接（默认端口 9001）")
    parser.add_argument("-t", "--topic", help="使用的主题，默认 lovetv/demo/xxxx")
    parser.add_argument("--listen", action="store_true", help="监听模式")
    parser.add_argument("--send", help="发送模式，例如 --send hello")
    parser.add_argument("--count", type=int, default=3, help="自测发送条数")
    parser.add_argument("--timeout", type=int, default=30, help="自测总超时秒数")
    args = parser.parse_args()
    if args.port is None:
        args.port = int(env.get("MQTT_PORT", "1883"))
        if args.ws:
            args.port = int(env.get("MQTT_WS_PORT", "9001"))

    if args.listen:
        return run_listen(args, env)
    if args.send:
        return run_send(args, env)
    return run_selftest(args, env)


if __name__ == "__main__":
    sys.exit(main())
