/**
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

// ----------------------- Clock definitions

#include <stdio.h>
#include <math.h>
#include "pico/time.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/rtc.h"
#include "pico/util/datetime.h"

static volatile bool fired = false;
const uint RED_PIN = 2;
const uint GEEN_PIN = 5;
const uint MOSFET = 3;
const int sleep = 10;
bool up = false;
static int fade = 0;
int maxFade = 0;
int full_fade_period = 30; // 1 - 65535, would mean either 1 sec to full fade or 65535 to full fade. 1800 is 30 minutes
int fade_speed = 1;
int fade_step = 1;
bool fading = false;
int averaging_size = 1000;
bool knob_regulating = false;
bool server_regulating = false;
int knob_diff = 0;

datetime_t t = {
    .year = 2020,
    .month = 01,
    .day = 13,
    .dotw = 3, // 0 is Sunday, so 3 is Wednesday
    .hour = 15,
    .min = 56,
    .sec = 10};

datetime_t fade_up = {
    .year = 2020,
    .month = 01,
    .day = 13,
    .dotw = 3, // 0 is Sunday, so 3 is Wednesday
    .hour = 06,
    .min = 40,
    .sec = 29};

datetime_t fade_down = {
    .year = 2020,
    .month = 01,
    .day = 13,
    .dotw = 3, // 0 is Sunday, so 3 is Wednesday
    .hour = 20,
    .min = 30,
    .sec = 59};

datetime_t alarm = {
    .year = -1,
    .month = -1,
    .day = -1,
    .dotw = -1,
    .hour = -1,
    .min = -00,
    .sec = -1};

datetime_t tNew = {
    .year = 2020,
    .month = 01,
    .day = 13,
    .dotw = 3, // 0 is Sunday, so 3 is Wednesday
    .hour = 11,
    .min = 20,
    .sec = 00};

// -------------------- functions

void check_fade_speed()
{
    // full fade period - 1-65535 (*seconds) - obrained from client
    // fade speed - fade increase per second
    fade_speed = 65535 / full_fade_period; // 1 - 1000?
    // fade step - fade increase every milisecond, needs to be divisble by 1000 and integer
    fade_step = 1000 / fade_speed;
    printf("Full Fade Period: %i, Fade step: %i, Fade speed: %i \n", full_fade_period, fade_step, fade_speed);
}

void on_pwm_wrap()
{
    // Clear the interrupt flag that brought us here
    pwm_clear_irq(pwm_gpio_to_slice_num(RED_PIN));
    pwm_clear_irq(pwm_gpio_to_slice_num(MOSFET));
    pwm_set_gpio_level(RED_PIN, fade);
    pwm_set_gpio_level(MOSFET, fade);
}

// -------------------------------- server definitions

#define TCP_PORT 4242
#define DEBUG_printf printf
#define BUF_SIZE 2048
#define TEST_ITERATIONS 10
#define POLL_TIME_S 5
#define WIFI_SSID "#Telia-05A2D6"
#define WIFI_PASSWORD "Mka91keMsmrst7d8"

typedef struct TCP_SERVER_T_
{
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
    bool complete;
    uint8_t buffer_sent[BUF_SIZE];
    uint8_t buffer_recv[BUF_SIZE];
    int sent_len;
    int recv_len;
    int run_count;
} TCP_SERVER_T;

static TCP_SERVER_T *tcp_server_init(void)
{
    TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
    if (!state)
    {
        DEBUG_printf("A SddDSADfailed to allocate state\n");
        return NULL;
    }
    return state;
}

static err_t tcp_server_close(void *arg)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    err_t err = ERR_OK;
    if (state->client_pcb != NULL)
    {
        tcp_arg(state->client_pcb, NULL);
        tcp_poll(state->client_pcb, NULL, 0);
        tcp_sent(state->client_pcb, NULL);
        tcp_recv(state->client_pcb, NULL);
        tcp_err(state->client_pcb, NULL);
        err = tcp_close(state->client_pcb);
        if (err != ERR_OK)
        {
            DEBUG_printf("close failed %d, calling abort\n", err);
            tcp_abort(state->client_pcb);
            err = ERR_ABRT;
        }
        state->client_pcb = NULL;
    }
    if (state->server_pcb)
    {
        tcp_arg(state->server_pcb, NULL);
        tcp_close(state->server_pcb);
        state->server_pcb = NULL;
    }
    return err;
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    DEBUG_printf("tcp_server_sent %u\n", len);
    state->sent_len += len;

    if (state->sent_len >= BUF_SIZE)
    {

        // We should get the data back from the client
        state->recv_len = 0;
        DEBUG_printf("Waiting for buffer from client\n");
    }

    return ERR_OK;
}

err_t tcp_server_send_data(void *arg, struct tcp_pcb *tpcb)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    // for (int i = 0; i < BUF_SIZE; i++)
    // {
    //     state->buffer_sent[i] = 3;
    // }
    state->buffer_sent[0] = 3;

    state->sent_len = 0;
    DEBUG_printf("Writing %ld bytes to client\n", BUF_SIZE);
    // this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
    // can use this method to cause an assertion in debug mode, if this method is called when
    // cyw43_arch_lwip_begin IS needed
    cyw43_arch_lwip_check();
    err_t err = tcp_write(tpcb, state->buffer_sent, BUF_SIZE, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        DEBUG_printf("Failed to write data %d\n", err);
        // return tcp_server_result(arg, -1);
        return false;
    }
    return ERR_OK;
}

err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    char str[4096] = {0};
    // char str_bod[4096] = {0};
    memcpy(str, p->payload, p->len);
    // memcpy(str_bod, p->payload, p->len);
    if (!p)
    {
        // return tcp_server_result(arg, -1);
        // DEBUG_printf("No idea why, but it gets here \n");
        return ERR_OK;
    }
    // this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
    // can use this method to cause an assertion in debug mode, if this method is called when
    // cyw43_arch_lwip_begin IS needed
    cyw43_arch_lwip_check();
    if (p->tot_len > 0)
    {
        // DEBUG_printf("tcp_server_recv %d/%d err %d\n", p->tot_len, state->recv_len, err);
        DEBUG_printf("Data received, payload: %s \n", str);
        // DEBUG_printf("Data received, payload BODY: %s \n", str_bod);
        // DEBUG_printf("Data received, payload: WOWAWOWIWIOAKWOKOWAKOKWAOKAW");
        // char hh = "";
        // char min = "";
        // DEBUG_printf("hh: %c, min: %c \n", hh, min);

        // int len = strlen(str);

        ///////////////////////////////////////////////////
        char hh_substring[3];
        char min_substring[3];
        char ss_substring[3];
        bool reset_time = true;

        strncpy(hh_substring, &str[2], 2);
        t.hour = atoi(hh_substring);
        strncpy(min_substring, &str[9], 2);
        t.min = atoi(min_substring);
        strncpy(ss_substring, &str[14], 2);
        t.sec = atoi(ss_substring);

        char a_hh_substring[3];
        char a_min_substring[3];
        char a_ss_substring[3];

        strncpy(a_hh_substring, &str[19], 2);
        fade_up.hour = atoi(a_hh_substring);
        strncpy(a_min_substring, &str[26], 2);
        fade_up.min = atoi(a_min_substring);
        strncpy(a_ss_substring, &str[31], 2);
        fade_up.sec = atoi(a_ss_substring);

        char fade_substring[6];

        strncpy(fade_substring, &str[36], 5);
        full_fade_period = atoi(fade_substring);

        char set_immediately_substring[2];
        strncpy(set_immediately_substring, &str[46], 1);
        char immediate_fade_substring[6];
        strncpy(immediate_fade_substring, &str[51], 5);
        char yes[2] = "y";
        char no[2] = "y";
        // DEBUG_printf("Set immediately? : %s, immediate fade: %s Comapring to: %s",
        //  &set_immediately_substring[0], &immediate_fade_substring, &compare[0]);

        // /////////////////////////////////////////////////

        check_fade_speed();

        if (set_immediately_substring[0] == yes[0])
        {
            server_regulating = true;
            knob_regulating = false;
            DEBUG_printf("Received instruction to reset fade immediately : %c, setting fade to: %c",
                         &set_immediately_substring, &immediate_fade_substring);
            fading = false;
            maxFade = atoi(immediate_fade_substring);
        }
        else if (set_immediately_substring[0] == no[0])
        {
            server_regulating = false;
        }

        if (reset_time)
        {
            DEBUG_printf("Resetting time to: %i:%i:%i \n", t.hour, t.min, t.sec);
            rtc_set_datetime(&t);
        }

        // Receive the buffer
        const uint16_t buffer_left = BUF_SIZE - state->recv_len;
        state->recv_len += pbuf_copy_partial(p, state->buffer_recv + state->recv_len,
                                             p->tot_len > buffer_left ? buffer_left : p->tot_len, 0);
        tcp_recved(tpcb, p->tot_len);
        tpcb = NULL;
    }
    pbuf_free(p);

    // Have we have received the whole buffer
    if (state->recv_len == BUF_SIZE)
    {

        // check it matches
        if (memcmp(state->buffer_sent, state->buffer_recv, BUF_SIZE) != 0)
        {
            DEBUG_printf("buffer mismatch\n");
            // return tcp_server_result(arg, -1);
        }
        DEBUG_printf("tcp_server_recv buffer ok\n");

        // Test complete?
        // state->run_count++;
        // if (state->run_count >= TEST_ITERATIONS)
        // {
        //     tcp_server_result(arg, 0);
        //     return ERR_OK;
        // }

        // Send another buffer
        return tcp_server_send_data(arg, state->client_pcb);
    }
    return ERR_OK;
}

// static err_t tcp_server_poll(void *arg, struct tcp_pcb *tpcb)
// {
//     DEBUG_printf("tcp_server_poll_fn\n");
//     // return tcp_server_result(arg, -1); // no response is an error?
//     return false;
// }

static void tcp_server_err(void *arg, err_t err)
{
    if (err != ERR_ABRT)
    {
        DEBUG_printf("tcp_client_err_fn %d\n (14 = connection reset)", err);
        // tcp_server_result(arg, err);
    }
}

static err_t tcp_server_accept(void

                                   *arg,
                               struct tcp_pcb *client_pcb, err_t err)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    if (err != ERR_OK || client_pcb == NULL)
    {
        DEBUG_printf("Failure in accept\n");
        // tcp_server_result(arg, err);
        return ERR_VAL;
    }
    DEBUG_printf("Client connected\n");

    state->client_pcb = client_pcb;
    tcp_arg(client_pcb, state);
    tcp_sent(client_pcb, tcp_server_sent);
    tcp_recv(client_pcb, tcp_server_recv);
    // tcp_poll(client_pcb, tcp_server_poll, POLL_TIME_S * 2);
    tcp_err(client_pcb, tcp_server_err);

    // DEBUG_printf("Received something i guess: buffer_recv %u, buffer_sent  %u \n", state->buffer_recv[0], state->buffer_sent[0]);
    return ERR_OK;
    // return tcp_server_send_data(arg, state->client_pcb);
}

static bool tcp_server_open(void *arg)
{
    TCP_SERVER_T *state = (TCP_SERVER_T *)arg;
    DEBUG_printf("Starting server at %s on port %u\n", ip4addr_ntoa(netif_ip4_addr(netif_list)), TCP_PORT);

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb)
    {
        DEBUG_printf("failed to create pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, NULL, TCP_PORT);
    if (err)
    {
        DEBUG_printf("failed to bind to port %u\n", TCP_PORT);
        return false;
    }

    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!state->server_pcb)
    {
        DEBUG_printf("failed to listen\n");
        if (pcb)
        {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_server_accept);

    return true;
}

static bool my_set_time(datetime_t t)
{
    rtc_init();
    rtc_set_datetime(&t);
    return true;
}

int main()
{
    stdio_init_all();

    if (cyw43_arch_init())
    {
        printf("failed to initialise\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();

    printf("Connecting to Wi-Fi...\n");
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000))
    {
        printf("failed to connect.\n");
        return 1;
    }
    else
    {
        printf("Connected.\n");
    }

    TCP_SERVER_T *state = tcp_server_init();
    tcp_server_open(state);

    // -----------------------ligtinh stuff
    my_set_time(t);

    stdio_init_all();

    adc_init();
    adc_gpio_init(26);
    adc_select_input(0);

    gpio_init(RED_PIN);
    gpio_set_function(RED_PIN, GPIO_FUNC_PWM);

    uint slice_num = pwm_gpio_to_slice_num(RED_PIN);
    pwm_clear_irq(slice_num);
    pwm_set_irq_enabled(slice_num, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, on_pwm_wrap);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 4.f);
    pwm_init(slice_num, &config, true);
    gpio_init(GEEN_PIN);
    gpio_set_dir(GEEN_PIN, 3);
    gpio_init(MOSFET);
    gpio_set_function(MOSFET, GPIO_FUNC_PWM);
    uint slice_num2 = pwm_gpio_to_slice_num(MOSFET);
    pwm_clear_irq(slice_num2);
    pwm_set_irq_enabled(slice_num2, true);

    pwm_init(slice_num2, &config, true);

    char datetime_buf[256];
    char *datetime_str = &datetime_buf[0];

    // char *s = "";

    int8_t second_counter = 0;
    // uint counter = 0;

    // ---------------------merged main loop
    // knob potentiometer control variables
    int loop_num = 0;
    int adc_readings[averaging_size];
    bool start_averaging = false;

    int sum = 0;
    int avg = 0;
    int fade_maxfade_diff = 0;

    check_fade_speed();

    while (state)
    {
        // int fade_speed = 1000 / full_fade_period; // 1 - 1000?
        // int fade_step = 1000 / fade_speed;

        if (!state)
        {
            // restart
        }
        cyw43_arch_poll();
        // sleep_ms(1000);
        // tight_loop_contents();

        // ---------------------------- lighting loop part

        uint32_t RESULT = adc_read();
        rtc_get_datetime(&t);
        datetime_to_str(datetime_str, sizeof(datetime_buf), &t);
        // printf("\r%s Voltage reading: %u Fade: %i, UP?:%d, Fading?:%d, KNOB_reg?:%d, Server_reg?:%d , MAX Fade: %i, AVERAGE?: %d \n",
        //        datetime_str, RESULT, fade, up, fading, knob_regulating, server_regulating, maxFade, avg);
        // printf("\r Alarm time: %i:%i:%i \n",
        //        fade_up.hour, fade_up.min, fade_up.sec);

        if ((t.hour == fade_up.hour) && (t.min == fade_up.min))
        // if ((t.sec == fade_up.sec))
        {
            fading = true;
            up = true;
            // printf("Fade up should trigger");
        }
        else if ((t.hour == fade_down.hour) && (t.min == fade_down.min))
        // else if ((t.sec == fade_down.sec))
        {
            fading = true;
            up = false;
            // printf("Fade down should trigger");
        }

        adc_readings[loop_num] = RESULT;
        if (loop_num == averaging_size)
        {
            loop_num = 0;
            start_averaging = true;
        }

        if (start_averaging)
        {
            for (int i = 0; i <= averaging_size; i++)
            {
                sum += adc_readings[i];
            }
            avg = sum / averaging_size;
            sum = 0;
        }

        loop_num++;

        if (avg >= 100)
        {
            // knob_diff = avg;
            // // sleep_ms(50);
            // if (abs(knob_diff - avg) > 3)
            // {

            // }
            maxFade = avg * 16;
            fading = false;
            server_regulating = false;
            knob_regulating = true;
        }
        else if (avg < 100)
        {
            if (RESULT > 500)
            {
                fading = false;
                knob_regulating = true;
            }
            knob_regulating = false;
            if (!server_regulating && !fading && fade > 0)
            {
                maxFade = 0;
                // fade--;
                // if (fade < 0)
                // {
                //     fade = 0;
                // }
                // if (second_counter != t.sec)
                // {
                //     second_counter = t.sec;
                //     maxFade = 0;
                //     int n = 0;
                //     fade--;
                // while (n < 1000)
                // {

                //     n = n + 10;
                //     sleep_ms(10);
                //     fade--;
                //     if (fade < maxFade)
                //     {
                //         fade = maxFade;
                //     }
                //     // printf("Fading down");
                // }
                // }
            }
        }

        if (!knob_regulating)
        {
            if (up && fading)
            {
                // if (RESULT > 3850 || fired == true)
                if (second_counter != t.sec)
                {
                    second_counter = t.sec;

                    maxFade = maxFade + fade_speed;
                    if (maxFade > 65535)
                    {
                        maxFade = 65535;
                        // fading = false;
                    }
                    // int n = 0;
                    // while (n < 1000)
                    // {

                    //     n = n + fade_step;
                    //     sleep_ms(fade_step);
                    //     fade++;
                    //     if (fade > maxFade)
                    //     {
                    //         fade = maxFade;
                    //     }
                    //     // printf("Fading up");
                    // }
                }

                // gpio_put(GEEN_PIN, 1);
                // s = "going Up";
            }
            if (!up && fading)
            {
                if (second_counter != t.sec)
                {
                    second_counter = t.sec;
                    maxFade = maxFade - fade_speed;
                    if (maxFade <= 0)
                    {
                        maxFade = 0;
                        fading = false;
                    }
                }
                // gpio_put(GEEN_PIN, 0);
                // s = "going down";
            }
        }

        fade_maxfade_diff = abs(maxFade - fade);
        if (fade < maxFade)
        {
            if (fading)
            {
                int n = 0;
                while (n < 1000)
                {

                    n = n + fade_step;
                    sleep_ms(fade_step);
                    fade++;
                    if (fade > maxFade)
                    {
                        fade = maxFade;
                    }
                }
            }
            else
            {
                fade = fade + sqrt(fade_maxfade_diff);
            }
        }
        else if (fade > maxFade)
        {
            if (fading)
            {
                int n = 0;
                while (n < 1000)
                {

                    n = n + fade_step;
                    sleep_ms(fade_step);
                    fade--;
                    if (fade < maxFade)
                    {
                        fade = maxFade;
                    }
                }
            }
            else
            {
                fade = fade - sqrt(fade_maxfade_diff);
            }
        }
    }
    DEBUG_printf("State failed, loop ended i guess");
    // run_tcp_server_test();
    // cyw43_arch_deinit();
    return 0;
}
