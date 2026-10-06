from flask import Flask, render_template, jsonify, request
import os
import time
import serial
import threading
from datetime import datetime
import requests

app = Flask(__name__)

UPLOAD_FOLDER = "static/profile"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# 모드 정의 (SMART, ROUTINE, MANUAL, EMERGENCY)
current_mode = "SMART"

# [상태 정의] 4개의 모터 및 장치 상태 세분화 정의
device_states = {
    "led": False,
    "buzzer": False,
    "motor1": False,  # 커튼 up
    "motor2": False,  # 커튼 down
    "motor3": False,  # 침대 up
    "motor4": False   # 침대 down
}

# index.html의 기존 자바스크립트가 기대하는 정확한 변수명 매칭
latest_sensor_data = {
    "temperature": 0.0,     
    "humidity": 0.0,        
    "cds": 0,               
    "gas": "정상",           
    "emergency_msg": ""     
}

profile = {
    "name": "User",
    "image": ""
}

activity_log = []
routines = []  

# 파이썬 웹 로그창에 "비상모드 발동"이 중복 적재되는 것을 막기 위한 플래그
emergency_logged = False

# 시리얼 포트를 프로그램 시작 시 전역 객체로 단 한 번만 오픈합니다.
try:
    ser = serial.Serial('/dev/ttyUSB1', 115200, timeout=0.2)
    print("✅ [성공] 송신용 시리얼 포트(/dev/ttyUSB1, 115200)가 상시 오픈되었습니다.")
except Exception as e:
    ser = None
    print("❌ [경고] 송신용 시리얼 포트를 열 수 없습니다. 케이블 연결을 확인하세요:", e)

def run(cmd):
    global ser
    if ser and ser.is_open:
        try:
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            time.sleep(0.01) # 하드웨어 타이밍 버퍼 확보
            
            clean_cmd = f"{cmd.strip()}\n"
            ser.write(clean_cmd.encode('utf-8'))
            ser.flush()  
            print(f"🚀 [시리얼 전송 성공]: {cmd.strip()}")
        except Exception as e:
            print(f"❌ [시리얼 전송 에러]: {e}")
    else:
        print("❌ [시리얼 차단] 포트가 닫혀 있어 명령을 보낼 수 없습니다.")
    return ""

def add_log(message):
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    activity_log.insert(0, f"[{now}] {message}")
    if len(activity_log) > 100:
        activity_log.pop()

def bg_routine_motor_fix(motor_key, cmd_on, delay_time, r_data):
    global device_states
    
    # 1. 웹 UI에 모터가 켜졌다고 표시
    device_states[motor_key] = True
    run(cmd_on)
    
    # 2. C 보드가 usleep으로 완전히 잠들고 모터가 구동되는 시간 동안 파이썬도 대기
    time.sleep(delay_time)
    
    # 3. [핵심] C 보드가 usleep에서 깨어나 버퍼를 밀어버린 직후, 
    # 원래 켜져 있어야 할 LED와 부저 명령을 쐐기 패킷으로 한 번 더 강제 전송합니다.
    print(f"🔄 [C보드 깨어남 대응] 모터 구동 종료 후 LED/부저 상태 재고정")
    run("led_on" if r_data.get("led", False) else "led_off")
    time.sleep(0.05)
    run("buzzer_on" if r_data.get("buzzer", False) else "buzzer_off")
    
    # 4. 모터 구동이 완전히 끝났으므로 웹 UI 배지를 False(꺼짐)로 안전하게 복구
    device_states[motor_key] = False
    add_log(f"루틴 백그라운드: {motor_key} 작동 및 UI 동기화 완료")

def routine_scheduler_loop():
    last_checked_minute = ""
    while True:
        global current_mode, device_states
        now = datetime.now()
        current_time_str = now.strftime("%H:%M")
        
        if current_time_str != last_checked_minute:
            if current_mode == "ROUTINE":
                for r in routines:
                    if r["time"] == current_time_str:
                        add_log(f"[루틴 실행] 지정 시간 완료 ({current_time_str})")
                        
                        # 1. LED 상태 반영 및 전송 후 C 보드가 처리할 시간을 줍니다.
                        device_states["led"] = r.get("led", False)
                        run("led_on" if device_states["led"] else "led_off")
                        time.sleep(0.3)  # 0.3초의 물리적 여유 제공 (버퍼 엉킴 방지)
                        
                        # 2. 부저 상태 반영 및 전송
                        device_states["buzzer"] = r.get("buzzer", False)
                        run("buzzer_on" if device_states["buzzer"] else "buzzer_off")
                        time.sleep(0.3)  # 0.3초 여유 제공

                        # 3. 모터 구동 (비동기 쓰레드가 C 보드의 블로킹 상태를 모니터링하며 UI 상태를 유지함)
                        if r.get("motor1", False):
                            threading.Thread(target=bg_routine_motor_fix, args=("motor1", "motor1_on", 0.5, r), daemon=True).start()
                            time.sleep(0.1)

                        if r.get("motor2", False):
                            threading.Thread(target=bg_routine_motor_fix, args=("motor2", "motor2_on", 0.5, r), daemon=True).start()
                            time.sleep(0.1)

                        # 침대 up 모터 (C 보드가 2.5초간 usleep으로 잠드는 구간 대응)
                        if r.get("motor3", False):
                            threading.Thread(target=bg_routine_motor_fix, args=("motor3", "motor3_on", 2.8, r), daemon=True).start()
                            time.sleep(0.1)

                        if r.get("motor4", False):
                            threading.Thread(target=bg_routine_motor_fix, args=("motor4", "motor4_on", 0.5, r), daemon=True).start()
                            
            last_checked_minute = current_time_str
        time.sleep(1)

# 백그라운드 타이머 스레드 가동
scheduler_thread = threading.Thread(target=routine_scheduler_loop, daemon=True)
scheduler_thread.start()

@app.route('/')
def index():
    return render_template("index.html")

@app.route('/mode')
def mode():
    return current_mode

@app.route('/device_states')
def get_device_states():
    global device_states
    return jsonify(device_states)

@app.route('/set_mode/<mode_type>')
def set_mode(mode_type):
    global current_mode
    if mode_type == "smart":
        current_mode = "SMART"
        add_log("SMART HOME MODE 전환")
    elif mode_type == "routine":
        current_mode = "ROUTINE"
        add_log("ROUTINE MODE 전환")
    elif mode_type == "manual":
        current_mode = "MANUAL"
        add_log("MANUAL MODE 전환")
    return jsonify({"status": "OK", "mode": current_mode})

@app.route('/emergency_on')
def emergency_on():
    global current_mode, device_states, emergency_logged
    current_mode = "EMERGENCY"
    device_states["led"] = True
    device_states["buzzer"] = True
    device_states["motor1"] = False
    device_states["motor2"] = False
    device_states["motor3"] = False
    device_states["motor4"] = False
    run("led_on")
    run("buzzer_on")
    
    if not emergency_logged:
        add_log("비상모드 발동")
        emergency_logged = True
    return "OK"

@app.route('/emergency_off')
def emergency_off():
    global current_mode, device_states, latest_sensor_data, emergency_logged
    
    try:
        requests.get("http://192.168.55.4:8080/emergency_off", timeout=1.0)
        print("🚀 [네트워크 전송 성공] 탑스트 보드 8080 포트로 직접 비상 해제 성공")
    except Exception as e:
        print("❌ [네트워크 에러] 탑스트 보드 8080 포트 연결 실패 (시리얼 우회 시도):", e)
        run("clear_emergency") 
    
    current_mode = "MANUAL"  
    device_states["led"] = False
    device_states["buzzer"] = False
    device_states["motor1"] = False
    device_states["motor2"] = False
    device_states["motor3"] = False
    device_states["motor4"] = False
    
    latest_sensor_data["emergency_msg"] = ""
    latest_sensor_data["gas"] = "정상"  
    if latest_sensor_data["temperature"] >= 30.0:
        latest_sensor_data["temperature"] = 24.5  
        
    emergency_logged = False  
    add_log("EMERGENCY MODE 종료")
    return "OK"

@app.route('/led/<state>')
def led(state):
    global current_mode, device_states
    current_mode = "MANUAL"
    device_states["led"] = (state == "on")
    run(f"led_{state}")
    add_log(f"LED {state.upper()}")
    return "OK"

@app.route('/buzzer/<state>')
def buzzer(state):
    global current_mode, device_states
    current_mode = "MANUAL"
    device_states["buzzer"] = (state == "on")
    run(f"buzzer_{state}")
    add_log(f"BUZZER {state.upper()}")
    return "OK"

@app.route('/motor1/<state>')
def motor1(state):
    global current_mode, device_states
    current_mode = "MANUAL"
    if state == "on":
        run("motor2_off")
        device_states["motor2"] = False
        
        device_states["motor1"] = True
        run("motor1_on")
        add_log("커튼 up")
        
        time.sleep(0.15)
        device_states["motor1"] = False 
    else:
        device_states["motor1"] = False
        run("motor1_off")
        add_log("커튼 up 정지")
    return "OK"

@app.route('/motor2/<state>')
def motor2(state):
    global current_mode, device_states
    current_mode = "MANUAL"
    if state == "on":
        run("motor1_off")
        device_states["motor1"] = False
        
        device_states["motor2"] = True
        run("motor2_on")
        add_log("커튼 down")
        
        time.sleep(0.15)
        device_states["motor2"] = False 
    else:
        device_states["motor2"] = False
        run("motor2_off")
        add_log("커튼 down 정지")
    return "OK"

@app.route('/motor3/<state>')
def motor3(state):
    global current_mode, device_states
    current_mode = "MANUAL"
    if state == "on":
        run("motor4_off")
        device_states["motor4"] = False
        
        device_states["motor3"] = True
        run("motor3_on")
        add_log("침대 up")
        
        time.sleep(2.5)
        device_states["motor3"] = False
    else:
        device_states["motor3"] = False
        run("motor3_off")
        add_log("침대 up 정지 ")
    return "OK"

@app.route('/motor4/<state>')
def motor4(state):
    global current_mode, device_states
    current_mode = "MANUAL"
    if state == "on":
        run("motor3_off")
        device_states["motor3"] = False
        
        device_states["motor4"] = True
        run("motor4_on")
        add_log("침대 down ")
        
        time.sleep(0.15)
        device_states["motor4"] = False
    else:
        device_states["motor4"] = False
        run("motor4_off")
        add_log("침대 down 정지 ")
    return "OK"

@app.route('/all/<state>')
def all_device(state):
    global current_mode, device_states
    current_mode = "MANUAL"
    is_on = (state == "on")
    device_states["led"] = is_on
    device_states["buzzer"] = is_on
    device_states["motor1"] = is_on
    device_states["motor2"] = is_on
    device_states["motor3"] = is_on
    device_states["motor4"] = is_on
    run(f"all_{state}")
    add_log(f"ALL DEVICE {state.upper()}")
    return "OK"

@app.route('/routines', methods=['GET'])
def get_routines():
    return jsonify(routines)

@app.route('/routines/add', methods=['POST'])
def add_routine():
    data = request.get_json()
    new_routine = {
        "id": int(time.time() * 1000),
        "time": data.get("time"),
        "led": data.get("led", False),
        "buzzer": data.get("buzzer", False),
        "motor1": data.get("motor1", False),
        "motor2": data.get("motor2", False),
        "motor3": data.get("motor3", False),
        "motor4": data.get("motor4", False)
    }
    routines.append(new_routine)
    add_log(f"새 루틴 추가 ({new_routine['time']})")
    return jsonify({"status": "OK"})

@app.route('/routines/delete/<int:routine_id>', methods=['DELETE'])
def delete_routine(routine_id):
    global routines
    routines = [r for r in routines if r['id'] != routine_id]
    add_log("루틴 삭제 완료")
    return jsonify({"status": "OK"})

@app.route('/profile')
def get_profile():
    return jsonify(profile)

@app.route('/profile/reset')
def reset_profile():
    profile["name"] = "User"
    profile["image"] = ""
    return "OK"

@app.route("/activity/add", methods=["POST"])
def add_activity():
    data = request.get_json()
    message = data.get("message", "기타 작업")
    add_log(message)
    return {"status": "ok"}

@app.route("/profile/save", methods=["POST"])
def save_profile():
    global profile
    old_name = profile["name"]
    new_name = request.form.get("name", "").strip()
    file = request.files.get("image")

    if new_name == "" or new_name == "User":
        new_name = "User"
    if new_name != old_name:
        profile["name"] = new_name

    if file and file.filename != "":
        filepath = os.path.join(UPLOAD_FOLDER, file.filename)
        file.save(filepath)
        profile["image"] = "/" + filepath
    return "OK"

@app.route('/activity/reset')
def reset_activity():
    global activity_log
    activity_log.clear()
    return "OK"

@app.route('/activity')
def get_activity():
    return jsonify(activity_log)

@app.route('/sensor')
def sensor():
    return jsonify(latest_sensor_data)

@app.route('/api/update_sensor', methods=['POST'])
def api_update_sensor():
    global current_mode, device_states, latest_sensor_data, emergency_logged
    
    data = request.get_json(silent=True)
    if not data:
        return jsonify({"status": "fail", "message": "No JSON data"}), 400
    
    try:
        latest_sensor_data["temperature"] = float(data.get("temperature", 0.0))
        latest_sensor_data["humidity"] = float(data.get("humidity", 0.0))
        latest_sensor_data["cds"] = int(data.get("cds", 0))
        
        gas_raw = int(data.get("gas", 1))
        latest_sensor_data["gas"] = "🚨 가스 누출!" if gas_raw == 0 else "정상"
        
        temp_val = latest_sensor_data["temperature"]
        is_c_emergency = (data.get("mode", "NORMAL") == "EMERGENCY") or (temp_val >= 30.0) or (gas_raw == 0)
        
        if temp_val <= 1.0:
            is_c_emergency = (gas_raw == 0)
        
        if is_c_emergency:
            msg_list = []
            if gas_raw == 0: msg_list.append("가스 누출 발생 경고")
            if temp_val >= 30.0: msg_list.append("화재 발생 경고")
            latest_sensor_data["emergency_msg"] = "\n".join(msg_list)
            
            if current_mode != "EMERGENCY":
                current_mode = "EMERGENCY"
                device_states["led"] = True
                device_states["buzzer"] = True
                device_states["motor1"] = False
                device_states["motor2"] = False
                device_states["motor3"] = False
                device_states["motor4"] = False
                
                run("led_on")
                run("buzzer_on")
        
                if not emergency_logged:
                    add_log("비상모드 발동 (보드 감지)")
                    emergency_logged = True
        else:
            if current_mode != "EMERGENCY":
                latest_sensor_data["emergency_msg"] = ""

        if current_mode == "SMART":
            light_val = latest_sensor_data["cds"]
            if light_val >= 80 and device_states["led"]:
                device_states["led"] = False
                run("led_off")
                add_log(f"스마트 제어: 조도({light_val}) >= 80 -> LED OFF")
            elif 0 <= light_val <= 10 and not device_states["led"]:
                device_states["led"] = True
                run("led_on")
                add_log(f"스마트 제어: 조도({light_val}) <= 10 -> LED ON")

        return jsonify({"status": "success", "web_mode": current_mode})

    except Exception as e:
        print("API 파싱 동기화 처리 도중 에러 발생:", e)
        return jsonify({"status": "error", "message": str(e)}), 500
    
if __name__ == '__main__':
    app.run(host='192.168.55.3', port=5002, debug=False)