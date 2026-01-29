#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cerrno>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <numeric>  // 추가된 헤더
#include <jsoncpp/json/json.h>  // JSON 처리 라이브러리

#define SERVO1_GPIO         89    // X축(좌우) 제어
#define SERVO_MIN_US      500    // 확장된 펄스 폭 하한
#define SERVO_MAX_US     2500    // 확장된 펄스 폭 상한
#define SERVO_NEUTRAL_US 1500
#define SERVO_PERIOD_US 20000
#define IMAGE_CENTER_CX   322
#define IMAGE_CENTER_CY   322
#define DEAD_ZONE         10
#define RED_LED_GPIO   "84"
#define GREEN_LED_GPIO "86"
#define YELLOW_LED_GPIO "85"
#define PORT             5000
#define MAX_RATE_US_PER_SEC 200.0f
#define FILTER_SIZE         5    // 이동평균 윈도우 크기

#define BUZZER_GPIO       "90"      // 부저 제어 핀 번호

std::atomic<bool> running(true), servo_active(false);
int pwm_x;
std::mutex lock_x;
std::chrono::steady_clock::time_point last_detect_time;

// clamp helper
static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// map angle->pulse
int map_pulse(int angle, int in_min, int in_max, int out_min, int out_max) {
    angle = clamp_int(angle, in_min, in_max);
    return (angle - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void export_gpio(const char* pin) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s", pin);
    if (access(path, F_OK) != 0) {
        int fd = open("/sys/class/gpio/export", O_WRONLY);
        if (fd >= 0) {
            write(fd, pin, strlen(pin));
            close(fd);
        }
        usleep(100000);
    }
}

void set_gpio_direction(const char* pin, const char* dir) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/direction", pin);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, dir, strlen(dir));
        close(fd);
    }
}

void set_gpio_value(const char* pin, int value) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/value", pin);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        write(fd, value ? "1" : "0", 1);
        close(fd);
    }
}

void init_leds() {
    export_gpio(RED_LED_GPIO);
    export_gpio(GREEN_LED_GPIO);
    export_gpio(YELLOW_LED_GPIO);
    set_gpio_direction(RED_LED_GPIO,   "out");
    set_gpio_direction(GREEN_LED_GPIO, "out");
    set_gpio_direction(YELLOW_LED_GPIO,"out");
}

void set_led_state(bool red, bool green, bool yellow) {
    set_gpio_value(RED_LED_GPIO,   red);
    set_gpio_value(GREEN_LED_GPIO, green);
    set_gpio_value(YELLOW_LED_GPIO,yellow);
}

// 부저 제어를 위한 함수 추가
void init_buzzer() {
    export_gpio(BUZZER_GPIO);
    set_gpio_direction(BUZZER_GPIO, "out");
}

void activate_buzzer_for_3_seconds() {
    set_gpio_value(BUZZER_GPIO, 1);  // 부저 켜기
    usleep(3000000);  // 3초 대기
    set_gpio_value(BUZZER_GPIO, 0);  // 부저 끄기
}

void servo_loop(int gpio, std::atomic<bool>& run_flag, std::mutex& lock, int& pwm_ref) {
    char buf[8];
    sprintf(buf, "%d", gpio);
    export_gpio(buf);
    set_gpio_direction(buf, "out");
    usleep(100000);

    int prev_pulse = SERVO_NEUTRAL_US;
    auto last_time = std::chrono::steady_clock::now();

    while (run_flag.load()) {
        if (!servo_active.load()) {
            usleep(SERVO_PERIOD_US);
            last_time = std::chrono::steady_clock::now();
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;

        int target;
        {
            std::lock_guard<std::mutex> guard(lock);
            target = pwm_ref;
        }

        int diff = target - prev_pulse;
        float max_step = MAX_RATE_US_PER_SEC * dt;
        diff = clamp_int(diff, -(int)max_step, (int)max_step);
        prev_pulse += diff;

        if (prev_pulse >= SERVO_MIN_US && prev_pulse <= SERVO_MAX_US) {
            set_gpio_value(buf, 1);
            usleep(prev_pulse);
            set_gpio_value(buf, 0);
            usleep(SERVO_PERIOD_US - prev_pulse);
        } else {
            set_gpio_value(buf, 0);
            usleep(SERVO_PERIOD_US);
        }
    }
}

void tcp_receive_loop() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}, cli;
    socklen_t cli_len = sizeof(cli);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 1);
    std::cout << "[TCP] Waiting for connection...\n";
    int client_fd = accept(server_fd, (sockaddr*)&cli, &cli_len);
    std::cout << "[TCP] Client connected!\n";

    timeval tv{0,500000};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    init_leds();
    set_led_state(true,false,false);
    last_detect_time = std::chrono::steady_clock::now();

    init_buzzer();  // 부저 초기화

    std::deque<int> dx_buf;
    char buffer[128];
    while (running.load()) {
        int len = read(client_fd, buffer, sizeof(buffer)-1);
        if (len > 0) {
            buffer[len] = '\0';
            float fcx, fcy;
            int cx, cy;
            if (sscanf(buffer, "%f,%f", &fcx, &fcy) == 2) {
                if (!servo_active.load()) {
                    int init_dx = int(fcx) - IMAGE_CENTER_CX;
                    int init_ax = clamp_int(90 - init_dx/4, -45, 225);
                    {
                        std::lock_guard<std::mutex> g(lock_x);
                        pwm_x = map_pulse(init_ax, -45, 225, SERVO_MIN_US, SERVO_MAX_US);
                    }
                    servo_active.store(true);
                }

                cx = int(fcx);
                cy = int(fcy);
                last_detect_time = std::chrono::steady_clock::now();

                int dx = cx - IMAGE_CENTER_CX;
                if (abs(dx) < DEAD_ZONE) dx = 0;

                // 이동평균 필터
                dx_buf.push_back(dx);
                if (dx_buf.size() > FILTER_SIZE) dx_buf.pop_front();
                int avg_dx = dx_buf.size()==FILTER_SIZE
                             ? std::accumulate(dx_buf.begin(), dx_buf.end(), 0) / FILTER_SIZE
                             : dx;

                // 각도 매핑 -30°..210°
                int angle_x = clamp_int(90 - avg_dx/4, -45, 225);

                bool need_move = (abs(avg_dx) > DEAD_ZONE);
                {
                    std::lock_guard<std::mutex> g(lock_x);
                    pwm_x = need_move
                          ? map_pulse(angle_x, -45, 225, SERVO_MIN_US, SERVO_MAX_US)
                          : SERVO_NEUTRAL_US;
                }

                if (need_move)        set_led_state(false,false,true);
                else if (servo_active) set_led_state(false,true,false);

                std::cout << "[TCP] cx="<<cx<<" avg_dx="<<avg_dx
                          <<" ax="<<angle_x<<"\n";
            }

            // 부저 신호 수신 처리
            if (strncmp(buffer, "BUZZ", 4) == 0) {
                std::cout << "[TCP] BUZZ command received.\n";
                activate_buzzer_for_3_seconds();  // 부저 3초 울리기
            }
        }
        else if (len<0 && (errno==EAGAIN||errno==EWOULDBLOCK)) {
            set_led_state(true,false,false);
            servo_active.store(false);
            dx_buf.clear();
            continue;
        }
        else {
            std::cout<<"[TCP] Disconnected or error\n";
            set_led_state(true,false,false);
            break;
        }
    }
    close(client_fd);
    close(server_fd);
}

cv::VideoCapture openCamera(int id) {
    cv::VideoCapture cap(id, cv::CAP_V4L2);
    if (!cap.isOpened()) std::cerr<<"Camera open error\n";
    return cap;
}

int main(){
    std::thread t1(tcp_receive_loop),
                t2(servo_loop,SERVO1_GPIO,std::ref(running),std::ref(lock_x),std::ref(pwm_x));

    cv::VideoCapture cap = openCamera(1);
    if(!cap.isOpened()) return -1;

    while(true){
        cv::Mat frame;
        cap>>frame;
        if(frame.empty()) break;
        cv::imshow("TOPST Camera", frame);
        if(cv::waitKey(30)==27) break;
    }

    running.store(false);
    t1.join(); t2.join();
    cap.release(); cv::destroyAllWindows();
    return 0;
}
