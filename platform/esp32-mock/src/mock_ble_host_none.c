/* The NimBLE mock's host Bluetooth backend where there is none to use
   (everywhere but macOS, see mock_ble_host.m): the simulated devices are
   all the firmware sees. */
#include "mock_ble_internal.h"

bool mock_ble_host_start(void) { return false; }
void mock_ble_host_scan(bool on, bool active, bool duplicates) { (void)on; (void)active; (void)duplicates; }
void mock_ble_host_connect(int link, const uint8_t addr[6])
{
    (void)addr;
    mock_ble_host_disconnected(link, 0x3E);   /* connection failed to be established */
}
void mock_ble_host_disconnect(int link) { (void)link; }
void mock_ble_host_read(int link, int chr) { (void)link; (void)chr; }
void mock_ble_host_write(int link, int chr, const uint8_t *data, size_t len, bool response)
{
    (void)link; (void)chr; (void)data; (void)len; (void)response;
}
void mock_ble_host_subscribe(int link, int chr, bool on) { (void)link; (void)chr; (void)on; }
