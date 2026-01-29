from flask import Flask, render_template, Response, request
import cv2
from ultralytics import YOLO
import threading
import queue
import socket
import time

app = Flask(__name__)
frame_queue = queue.Queue(maxsize=1)

# ────────── YOLO 모델 ──────────
model = YOLO('yolo11n.pt')

# ────────── 네트워크 설정 ──────────
TOPST_IP   = '192.168.0.102'
TOPST_PORT = 5000

# 부저 신호 저장용
buzzer_signal = ""

# ────────── 좌표 계산 & 메시지 생성 ──────────
def calculate_center(box):
    x1, y1, x2, y2 = box
    return int((x1 + x2) / 2), int((y1 + y2) / 2)

def create_center_message(boxes):
    lines = [f"{cx},{cy}\n" for (cx, cy) in [calculate_center(b) for b in boxes]]
    return "".join(lines)

# ────────── TCP 송신 헬퍼 ──────────
class TcpSender:
    def __init__(self, ip, port):
        self.ip, self.port = ip, port
        self.sock = None
        self._connect()

    def _connect(self):
        while True:
            try:
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.settimeout(2.0)
                self.sock.connect((self.ip, self.port))
                break
            except OSError:
                print('[INFO] TOPST 연결 재시도 …')
                time.sleep(1)

    def send(self, msg: str):
        if not msg:
            return
        try:
            self.sock.sendall(msg.encode())
        except OSError:
            # 재연결 후 재전송
            self.sock.close()
            self._connect()
            self.sock.sendall(msg.encode())

    def close(self):
        if self.sock:
            self.sock.close()

# ────────── 비디오 쓰레드 ──────────
def video_thread():
    cap = cv2.VideoCapture(4, cv2.CAP_V4L2)
    if not cap.isOpened():
        print("Error: Camera open failed")
        return

    sender = TcpSender(TOPST_IP, TOPST_PORT)

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                break

            roi         = frame[26:210, 0:184]
            roi_resized = cv2.resize(roi, (int(184 * 3.5), int(184 * 3.5)))

            # 객체 탐지
            results = model(roi_resized, classes=[0])
            boxes   = results[0].boxes.xyxy.cpu().numpy()

            # 좌표 전송
            msg = create_center_message(boxes)
            sender.send(msg)

            # 시각화
            annotated = results[0].plot()
            for box in boxes:
                cx, cy = calculate_center(box)
                cv2.circle(annotated, (cx, cy), 4, (0, 0, 255), -1)

            # 웹 스트림 프레임 저장
            try:
                frame_queue.get_nowait()
            except queue.Empty:
                pass
            try:
                frame_queue.put_nowait(annotated)
            except queue.Full:
                pass

            cv2.imshow("YOLO Tracking", annotated)
            if cv2.waitKey(1) == 27:  # ESC
                break
    finally:
        sender.close()
        cap.release()
        cv2.destroyAllWindows()

# ────────── 웹 스트리밍 ──────────
def gen_frames():
    while True:
        try:
            frame = frame_queue.get(timeout=1)
        except queue.Empty:
            continue
        ret, buffer = cv2.imencode('.jpg', frame)
        if not ret:
            continue
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' +
               buffer.tobytes() +
               b'\r\n')

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/video_feed')
def video_feed():
    return Response(gen_frames(),
                    mimetype='multipart/x-mixed-replace; boundary=frame')

# ────────── 부저 콘트롤 ──────────
def activate_buzzer():
    global buzzer_signal
    buzzer_signal = "BUZZ"

def send_buzzer_signal_to_tcp():
    if not buzzer_signal:
        return
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(2.0)
            sock.connect((TOPST_IP, TOPST_PORT))
            sock.sendall(buzzer_signal.encode())
            print("[INFO] 부저 신호 전송 완료")
    except Exception as e:
        print(f"[ERROR] 부저 신호 전송 실패: {e}")

@app.route('/buzz', methods=['POST'])
def buzz():
    activate_buzzer()
    send_buzzer_signal_to_tcp()
    return 'OK', 200

# ────────── 실행 ──────────
if __name__ == '__main__':
    threading.Thread(target=video_thread, daemon=True).start()
    app.run(host='0.0.0.0', port=5000, threaded=True)

