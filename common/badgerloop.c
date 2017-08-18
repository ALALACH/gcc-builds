#include <string.h>
#include "badgerloop.h"
#include "hal/stm32f7xx_hal.h"
#include "lwip/err.h"
#include "lwip/udp.h"
#include "ethernetif.h"
#include "cli.h"
#include "gpio.h"
#include "state_machine.h"
#include "adc.h"
#include "mpu9250.h"
#include "HWPressure.h"

/*****************************************************************************/
/*                                 Data Buffer                               */
/*****************************************************************************/
uint8_t telemetry_buffer[DASHBOARDP_SIZ];

/* Team ID, Status */
uint8_t *team_id = &telemetry_buffer[0];
uint8_t *status = &telemetry_buffer[1];

/* Position, Velocity, Acceleration */
int *acceleration = (int *) &telemetry_buffer[2],
	*position = (int *) &telemetry_buffer[6],
	*velocity = (int *) &telemetry_buffer[10],

	/* Battery Voltage and Current*/
	*v_batt = (int *) &telemetry_buffer[14],
	*i_batt = (int *) &telemetry_buffer[18],

	/* Battery Temperature, Pod Temperature */
	*t_batt = (int *) &telemetry_buffer[22],
	*t_pod = (int *) &telemetry_buffer[26];

/* Strip Count */
uint32_t *s_count = (uint32_t *) &telemetry_buffer[30];

/*====================== Dashboard Only Fields ==============================*/

/* Ambient Pressure */
uint16_t *p_amb = (uint16_t *) &telemetry_buffer[34],

/* Pressure Sensors */
	*pr_p1 = (uint16_t *) &telemetry_buffer[36],
	*pr_p2 = (uint16_t *) &telemetry_buffer[38],
	*br_p1 = (uint16_t *) &telemetry_buffer[40],
	*br_p2 = (uint16_t *) &telemetry_buffer[42],
	*br_p3 = (uint16_t *) &telemetry_buffer[44];

/* Limit Swtich States */
uint8_t *lim_states = &telemetry_buffer[46];
/*****************************************************************************/
/*****************************************************************************/

void badgerloop_update_data(void) {

	SET_STATUS(state_handle.curr_state);

	/* combination of strips and accelerometer */

	SET_ACCEL(readAccelXData());
	SET_VEL(-1);
	SET_POS(-4);

	/* I2C temp/pressure sensor */
	SET_PAMP(2);
/*	SET_PAMP(HPGetPressure());*/
	SET_TPOD(1);
/*	SET_TPOD(HPGetTemp());*/

	/* analog voltages */
	SET_VBATT(analogRead(ADC3, 13));	/* C3:   Analog5 - Primary Battery Voltage */
	SET_IBATT(analogRead(ADC3, 3));	/* A3:   Analog1 - Primary Battery Current */
	SET_TBATT(analogRead(ADC1, 4));	/* A4:  Analog14 - Thermistor 1 */
	SET_PRP1(analogRead(ADC3, 10));	/* C0:   Analog3 - Pressure 1 (CN5) */
	SET_PRP2(analogRead(ADC3, 12));	/* C2:   Analog4 - Pressure 2 (CN5) */
	SET_BRP1(analogRead(ADC3, 9));	/* F3:   Analog6 - Pressure 3 (CN5) */
	SET_BRP2(analogRead(ADC3, 14));	/* F4:   Analog7 - Pressure 4 (CN5) */
	SET_BRP3(analogRead(ADC3, 15));	/* F5:   Analog8 - Pressure 1 (CN6) */

	/* B1:   Analog2 - Secondary Battery Voltage */
	/* F9:  Analog13 - Secondary Battery Current */

	/* F10:  Analog9 - Pressure 2 (CN6) */

	/* F6:  Analog10 - Accelerometer X */

	/* A5:  Analog15 - Thermistor 2 */
	/* F7:  Analog11 - Thermistor 3 */
	/* F8:  Analog12 - Thermistor 4 */

	/* strip count */
	SET_SCOUNT(4);

	/*************************************************************************/
	/*                            digital I/O                                */
	/*************************************************************************/
	/* LIM1, braking limit switch 1 */
	if (gpio_readPin(GPIOG, 2)) SET_BLIM1;
	else CLR_BLIM1;

	/* LIM2, braking limit switch 2 */
	if (gpio_readPin(GPIOG, 3)) SET_BLIM2;
	else CLR_BLIM2;

	/* GPIO1, pusher limit switch 1 */
	if (gpio_readPin(GPIOD, 0)) SET_PLIM1;
	else CLR_PLIM1;

	/* GPIO2, pusher limit switch 2 */
	if (gpio_readPin(GPIOD, 1)) SET_PLIM2;
	else CLR_PLIM2;

	/* GPIO3, door limit switch? */
	if (gpio_readPin(GPIOD, 3)) SET_DLIM;
	else CLR_DLIM;
	/*************************************************************************/
	/*************************************************************************/
}

/* Networking */
ip_addr_t to_spacex, to_dashboard;
struct udp_pcb *udp_spacex, *udp_dashboard;
struct pbuf *spacex_payload, *dashboard_payload, *message_payload;

/* Globals */
uint32_t last_telem_timestamp, last_daq_timestamp;
state_t state_handle;
char *fault_message = "INITIAL_VAL";
static err_t lwip_error = ERR_OK;
static uint8_t badgerloop_flags = 0;

/* for accepting commands from the dashboard */
void udp_echo_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
	const ip_addr_t *dst_ip, u16_t dst_port) {
	//printf("got a packet from %"U16_F".%"U16_F".%"U16_F".%"U16_F" on port %d\r\n",
	//	ip4_addr1_16(dst_ip), ip4_addr2_16(dst_ip),
	//	ip4_addr3_16(dst_ip), ip4_addr4_16(dst_ip), dst_port);

	/* check for query response, it's highly unfortunate
	   we check this every time but the only way would
	   be to create some kind of tick window that checks
	   for a recent query because we "spam" the output
	   and may get this twice */
	if (!strcmp((char *) p->payload, "new phone who dis")) {
		if (badgerloop_flags & OUTGOING_QUERY)
			badgerloop_flags |= DASH_RESPONSE;
		return;
	}
	printf("%s\r\n", (char *) p->payload);
	process_input((char *) p->payload);
	pbuf_free(p);
	printPrompt();
}

int badgerloop_init(void) {

	/* don't initialize twice, only query dashboard */
	if (udp_spacex != NULL && udp_dashboard != NULL) {
		if (eth_check_link() && query_Dashboard())
			set_stdio_target(UDP);
		return 0;
	}

	/* TODO: make configurable */
	IP4_ADDR(&to_spacex, 192, 168, 0, DEV_IP);
	IP4_ADDR(&to_dashboard, 192, 168, 0, DEV_IP);

	udp_spacex = udp_new();
	udp_dashboard = udp_new();

	if (udp_spacex == NULL || udp_dashboard == NULL) return -1;

	lwip_error = udp_connect(udp_spacex, &to_spacex, 3000);
	if (lwip_error != ERR_OK) return -1;

	lwip_error = udp_connect(udp_dashboard, &to_dashboard, 3000);
	if (lwip_error != ERR_OK) return -1;

	lwip_error = udp_bind(udp_dashboard, &(gnetif.ip_addr), 3000);
	if (lwip_error != ERR_OK) return -1;

	udp_recv(udp_dashboard, udp_echo_recv, NULL);

	/* default values */
	*team_id = TEAM_ID;

	if (eth_check_link() && query_Dashboard())
		set_stdio_target(UDP);

	initialize_state_machine(&state_handle, IDLE,
		to_handlers, in_handlers, from_handlers,
		state_event_timestamps, state_intervals);

	/* initial capture */
	badgerloop_update_data();

	return 0;
}

int send_telemetry_to_SpaceX(void) {

	spacex_payload = pbuf_alloc(PBUF_TRANSPORT, SPACEXP_SIZ, PBUF_POOL);
	if (spacex_payload == NULL) return -1;

	memcpy(spacex_payload->payload, telemetry_buffer, SPACEXP_SIZ);

	last_telem_timestamp = ticks;

	lwip_error = udp_send(udp_spacex, spacex_payload);
	pbuf_free(spacex_payload);

	return (lwip_error != ERR_OK) ? -1 : 0;
}

int send_telemetry_to_Dashboard(void) {

	dashboard_payload = pbuf_alloc(PBUF_TRANSPORT, DASHBOARDP_SIZ, PBUF_POOL);
	if (dashboard_payload == NULL) return -1;

	memcpy(dashboard_payload->payload, telemetry_buffer, DASHBOARDP_SIZ);

	last_telem_timestamp = ticks;

	lwip_error = udp_send(udp_dashboard, dashboard_payload);
	pbuf_free(dashboard_payload);

	return (lwip_error != ERR_OK) ? -1 : 0;
}

int send_message_to_Dashboard(char *buf, int length) {

	message_payload = pbuf_alloc(PBUF_TRANSPORT, length + 5, PBUF_POOL);
	if (message_payload == NULL) return -1;

	/* indicate to dashboard this is a message */
	memcpy(message_payload->payload, "MSG: ", 5);
	memcpy(message_payload->payload + 5, buf, length);

	last_telem_timestamp = ticks;

	lwip_error = udp_send(udp_dashboard, message_payload);
	pbuf_free(message_payload);

	return (lwip_error != ERR_OK) ? -1 : 0;
}

static err_t sendQueryMessage(void) {
	err_t err;
	const char *message = "MSG: dashboard?";
	message_payload = pbuf_alloc(PBUF_TRANSPORT, strlen(message), PBUF_POOL);
	if (message_payload == NULL) return 0;
	memcpy(message_payload->payload, message, strlen(message));
	err = udp_send(udp_dashboard, message_payload);
	pbuf_free(message_payload);
	if (err != ERR_OK)
		printf("couldn't send again: %s\r\n", lwip_strerr(lwip_error));
	return err;
}

int query_Dashboard(void) {

	uint32_t query_start, last_retry_tick = 0, attempts = 0;

	query_start = ticks;
	badgerloop_flags |= OUTGOING_QUERY;

	/* blocking poll */
	do {
		lwip_loop_handler();

		/* check if response arrived */
		if (badgerloop_flags & DASH_RESPONSE) {
			badgerloop_flags &= ~(DASH_RESPONSE | OUTGOING_QUERY);
			return 1;
		}
		/* continue to send periodically */
		if (((ticks - query_start) % QUERY_RETRY == 0) && last_retry_tick != ticks) {
			last_retry_tick = ticks;
			attempts++;
			lwip_error = sendQueryMessage();
			if (lwip_error != ERR_OK)
				return 0;
		}
	} while (ticks - query_start < QUERY_TO);

	/* did not get a response */
	printf("%s: query timed out after %lu attempts\r\n", __func__, attempts);
	badgerloop_flags &= ~OUTGOING_QUERY;
	return 0;
}

void application_handler(void) {

	/* do DAQ */
	if (!(ticks % DAQ_INT) && last_daq_timestamp != ticks) {
		last_daq_timestamp = ticks;
		badgerloop_update_data();
	}

	/* do telemetry */
	if (!(ticks % TELEM_INT) && last_telem_timestamp != ticks) {
		last_telem_timestamp = ticks;
		if (gnetif.flags & NETIF_FLAG_LINK_UP) {
			if (!send_telemetry_to_SpaceX() || !send_telemetry_to_Dashboard()) {
				state_handle.next_state = FAULT;
				fault_message = "Sending telemetry failed!";
				state_handle.change_state = true;
			}
		/* No link, enter fault */
		} else {
			state_handle.next_state = FAULT;
			fault_message = "No network link!";
			state_handle.change_state = true;
		}
	}

	state_machine_handler(&state_handle);

}
