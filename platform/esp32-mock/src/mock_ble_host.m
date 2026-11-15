/*
 * ESP32 mock platform - the NimBLE mock's link to the computer's own
 * Bluetooth (macOS CoreBluetooth), so the simulator reaches real devices.
 *
 * CoreBluetooth works a level above NimBLE: it never shows a device's
 * address (each peripheral has a UUID of its own on this Mac), it hands over
 * advertisements already parsed, and it finds services and characteristics
 * as objects without attribute handles. This file turns that back into what
 * mock_ble.c expects: a made-up address per peripheral (random static, from
 * its UUID), the advertisement as AD structures again, and characteristics
 * by index. macOS pairs by itself when a device asks for it.
 *
 * Everything runs on one serial dispatch queue: the calls from mock_ble.c
 * are queued there and CoreBluetooth calls its delegate there.
 */
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <stdio.h>

#include "mock_ble_internal.h"

#define MAX_LINKS 3

@interface MockBleHost : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@end

@implementation MockBleHost {
@public
    CBCentralManager *_cm;
    dispatch_queue_t _q;
    NSMutableDictionary<NSUUID *, CBPeripheral *> *_seen;
    NSMutableDictionary<NSData *, NSUUID *> *_byAddr;
    BOOL _wantScan, _scanDups, _scanning;
    CBPeripheral *_link[MAX_LINKS];
    NSMutableArray<CBCharacteristic *> *_chrs[MAX_LINKS];
    NSMutableDictionary<NSNumber *, NSNumber *> *_reads[MAX_LINKS];   /* chr index -> reads waiting */
    int _pendingSvcs[MAX_LINKS];
}

static MockBleHost *s_host;

static void addr_of(NSUUID *ident, uint8_t out[6])
{
    uuid_t u;
    [ident getUUIDBytes:u];
    memcpy(out, u, 6);
    out[0] |= 0xc0;                   /* random static */
}

static int link_of(MockBleHost *h, CBPeripheral *p)
{
    for (int i = 0; i < MAX_LINKS; i++) if (h->_link[i] == p) return i;
    return -1;
}

static bool uuid_from_cb(CBUUID *cu, ble_uuid_any_t *out)
{
    NSData *d = cu.data;             /* big-endian */
    const uint8_t *b = d.bytes;
    if (d.length == 2) { out->u16.u.type = BLE_UUID_TYPE_16; out->u16.value = (uint16_t)(b[0] << 8 | b[1]); return true; }
    if (d.length == 4) {
        out->u32.u.type = BLE_UUID_TYPE_32;
        out->u32.value = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        return true;
    }
    if (d.length == 16) {
        out->u128.u.type = BLE_UUID_TYPE_128;
        for (int i = 0; i < 16; i++) out->u128.value[i] = b[15 - i];
        return true;
    }
    return false;
}

static int att_error(NSError *e)
{
    if (!e) return 0;
    if ([e.domain isEqualToString:CBATTErrorDomain] && e.code > 0 && e.code < 0x100) return (int)e.code;
    return BLE_ATT_ERR_UNLIKELY;
}

/* AD structure: [len][type][data]; false when out of room. */
static bool ad_put(uint8_t *ad, size_t *len, size_t cap, uint8_t type, const void *data, size_t n)
{
    if (n > 253 || *len + 2 + n > cap) return false;
    ad[(*len)++] = (uint8_t)(n + 1);
    ad[(*len)++] = type;
    memcpy(ad + *len, data, n);
    *len += n;
    return true;
}

/* A CBUUID as AD bytes (little-endian). */
static size_t uuid_le(CBUUID *cu, uint8_t out[16])
{
    NSData *d = cu.data;
    const uint8_t *b = d.bytes;
    for (NSUInteger i = 0; i < d.length && i < 16; i++) out[i] = b[d.length - 1 - i];
    return d.length;
}

- (instancetype)init
{
    if ((self = [super init])) {
        _q = dispatch_queue_create("luaboard.ble", DISPATCH_QUEUE_SERIAL);
        _seen = [NSMutableDictionary new];
        _byAddr = [NSMutableDictionary new];
        _cm = [[CBCentralManager alloc] initWithDelegate:self queue:_q options:nil];
    }
    return self;
}

- (void)updateScan
{
    BOOL on = _wantScan && _cm.state == CBManagerStatePoweredOn;
    if (on) {
        [_cm scanForPeripheralsWithServices:nil
                                    options:@{ CBCentralManagerScanOptionAllowDuplicatesKey : @(_scanDups) }];
    } else if (_scanning) {
        [_cm stopScan];
    }
    _scanning = on;
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    switch (central.state) {
    case CBManagerStatePoweredOn: fprintf(stderr, "ble: using this computer's Bluetooth\n"); break;
    case CBManagerStatePoweredOff: fprintf(stderr, "ble: this computer's Bluetooth is off\n"); break;
    case CBManagerStateUnauthorized:
        fprintf(stderr, "ble: no permission for Bluetooth (System Settings > Privacy & Security > Bluetooth)\n");
        break;
    case CBManagerStateUnsupported: fprintf(stderr, "ble: this computer has no Bluetooth LE\n"); break;
    default: break;
    }
    if (central.state != CBManagerStatePoweredOn) {
        _scanning = NO;
        for (int i = 0; i < MAX_LINKS; i++) {
            if (!_link[i]) continue;
            _link[i] = nil;
            mock_ble_host_disconnected(i, 0x08);
        }
    }
    [self updateScan];
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)p
     advertisementData:(NSDictionary<NSString *, id> *)adv RSSI:(NSNumber *)rssi
{
    _seen[p.identifier] = p;
    uint8_t addr[6];
    addr_of(p.identifier, addr);
    _byAddr[[NSData dataWithBytes:addr length:6]] = p.identifier;

    uint8_t ad[255];
    size_t len = 0;
    NSString *name = adv[CBAdvertisementDataLocalNameKey];
    if (name) {
        const char *s = name.UTF8String;
        ad_put(ad, &len, sizeof ad, 0x09, s, strlen(s));
    }
    NSData *mfg = adv[CBAdvertisementDataManufacturerDataKey];     /* starts with the company ID */
    if (mfg) ad_put(ad, &len, sizeof ad, 0xff, mfg.bytes, mfg.length);
    NSDictionary<CBUUID *, NSData *> *sd = adv[CBAdvertisementDataServiceDataKey];
    for (CBUUID *cu in sd) {
        uint8_t buf[253];
        size_t ul = uuid_le(cu, buf);
        NSData *v = sd[cu];
        if (ul + v.length > sizeof buf) continue;
        memcpy(buf + ul, v.bytes, v.length);
        ad_put(ad, &len, sizeof ad, ul == 2 ? 0x16 : ul == 4 ? 0x20 : 0x21, buf, ul + v.length);
    }
    NSArray<CBUUID *> *uuids = adv[CBAdvertisementDataServiceUUIDsKey];
    for (size_t size = 2; size <= 16; size *= 2) {
        uint8_t buf[240];
        size_t n = 0;
        for (CBUUID *cu in uuids)
            if (cu.data.length == size && n + size <= sizeof buf) n += uuid_le(cu, buf + n);
        if (n) ad_put(ad, &len, sizeof ad, size == 2 ? 0x03 : size == 4 ? 0x05 : 0x07, buf, n);
        if (size == 4) size = 8;      /* 2, 4, 16 */
    }
    NSNumber *tx = adv[CBAdvertisementDataTxPowerLevelKey];
    if (tx) {
        int8_t t = (int8_t)tx.intValue;
        ad_put(ad, &len, sizeof ad, 0x0a, &t, 1);
    }
    int r = rssi.intValue;
    if (r == 127) return;             /* "not available" */
    mock_ble_host_adv(addr, r, ad, len);
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)p
{
    int link = link_of(self, p);
    if (link < 0) { [_cm cancelPeripheralConnection:p]; return; }
    p.delegate = self;
    [p discoverServices:nil];
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)error
{
    int link = link_of(self, p);
    if (link < 0) return;
    _link[link] = nil;
    mock_ble_host_disconnected(link, 0x3e);
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)error
{
    int link = link_of(self, p);
    if (link < 0) return;
    _link[link] = nil;
    int reason = 0x16;                                     /* we closed it */
    if (error.code == CBErrorPeripheralDisconnected) reason = 0x13;
    else if (error) reason = 0x08;                         /* lost */
    mock_ble_host_disconnected(link, reason);
}

- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)error
{
    int link = link_of(self, p);
    if (link < 0) return;
    if (error || p.services.count == 0) {
        _chrs[link] = [NSMutableArray new];
        mock_ble_host_connected(link, NULL, 0);
        return;
    }
    _pendingSvcs[link] = (int)p.services.count;
    for (CBService *s in p.services) [p discoverCharacteristics:nil forService:s];
}

- (void)peripheral:(CBPeripheral *)p didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error
{
    int link = link_of(self, p);
    if (link < 0 || --_pendingSvcs[link] > 0) return;
    /* all services done: hand over the characteristics, grouped by service */
    _chrs[link] = [NSMutableArray new];
    _reads[link] = [NSMutableDictionary new];
    mock_ble_chr_t chrs[MOCK_BLE_MAX_CHRS];
    int n = 0;
    for (CBService *s in p.services) {
        ble_uuid_any_t su;
        if (!uuid_from_cb(s.UUID, &su)) continue;
        for (CBCharacteristic *c in s.characteristics) {
            if (n == MOCK_BLE_MAX_CHRS || !uuid_from_cb(c.UUID, &chrs[n].uuid)) continue;
            chrs[n].svc = su;
            chrs[n].props = (uint8_t)(c.properties & 0xff);
            [_chrs[link] addObject:c];
            n++;
        }
    }
    mock_ble_host_connected(link, chrs, n);
}

- (int)indexOf:(CBCharacteristic *)c link:(int)link
{
    NSUInteger i = [_chrs[link] indexOfObjectIdenticalTo:c];
    return i == NSNotFound ? -1 : (int)i;
}

- (void)peripheral:(CBPeripheral *)p didUpdateValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error
{
    int link = link_of(self, p), i = link < 0 ? -1 : [self indexOf:c link:link];
    if (i < 0) return;
    /* CoreBluetooth reports reads and notifications alike: a read waiting
       for this characteristic takes the value, else it's a notification. */
    NSNumber *k = @(i);
    int waiting = _reads[link][k].intValue;
    if (waiting > 0) {
        _reads[link][k] = @(waiting - 1);
        mock_ble_host_read_done(link, i, att_error(error), c.value.bytes, c.value.length);
    } else if (!error) {
        mock_ble_host_value(link, i, c.value.bytes, c.value.length);
    }
}

- (void)peripheral:(CBPeripheral *)p didWriteValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error
{
    int link = link_of(self, p), i = link < 0 ? -1 : [self indexOf:c link:link];
    if (i >= 0) mock_ble_host_write_done(link, i, att_error(error));
}

- (void)peripheral:(CBPeripheral *)p didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)c
             error:(NSError *)error
{
    int link = link_of(self, p), i = link < 0 ? -1 : [self indexOf:c link:link];
    if (i >= 0) mock_ble_host_subscribe_done(link, i, att_error(error));
}

@end

/* ---------------------------------------------------- mock_ble.c's calls */

static CBCharacteristic *chr_at(int link, int chr)
{
    if (link < 0 || link >= MAX_LINKS || !s_host->_link[link] || chr < 0 || chr >= (int)s_host->_chrs[link].count)
        return nil;
    return s_host->_chrs[link][(NSUInteger)chr];
}

bool mock_ble_host_start(void)
{
    if (!s_host) {
        if (@available(macOS 10.15, *)) {
            CBManagerAuthorization a = CBCentralManager.authorization;
            if (a == CBManagerAuthorizationDenied || a == CBManagerAuthorizationRestricted) {
                fprintf(stderr, "ble: no permission for Bluetooth (System Settings > Privacy & Security > Bluetooth)\n");
                return false;
            }
        }
        s_host = [MockBleHost new];
    }
    return true;
}

void mock_ble_host_scan(bool on, bool active, bool duplicates)
{
    dispatch_async(s_host->_q, ^{
        if (s_host->_scanning && on && s_host->_scanDups != duplicates) [s_host->_cm stopScan];
        s_host->_wantScan = on;
        s_host->_scanDups = duplicates;
        [s_host updateScan];
    });
}

void mock_ble_host_connect(int link, const uint8_t addr[6])
{
    NSData *key = [NSData dataWithBytes:addr length:6];
    dispatch_async(s_host->_q, ^{
        NSUUID *ident = s_host->_byAddr[key];
        CBPeripheral *p = ident ? s_host->_seen[ident] : nil;
        if (!p || link < 0 || link >= MAX_LINKS) return;     /* the connect times out */
        s_host->_link[link] = p;
        [s_host->_cm connectPeripheral:p options:nil];
    });
}

void mock_ble_host_disconnect(int link)
{
    dispatch_async(s_host->_q, ^{
        if (link < 0 || link >= MAX_LINKS || !s_host->_link[link]) return;
        CBPeripheral *p = s_host->_link[link];
        if (p.state == CBPeripheralStateConnecting) {
            s_host->_link[link] = nil;                      /* no callback comes for a cancelled attempt */
            [s_host->_cm cancelPeripheralConnection:p];
            mock_ble_host_disconnected(link, 0x16);
        } else {
            [s_host->_cm cancelPeripheralConnection:p];     /* didDisconnectPeripheral follows */
        }
    });
}

void mock_ble_host_read(int link, int chr)
{
    dispatch_async(s_host->_q, ^{
        CBCharacteristic *c = chr_at(link, chr);
        if (!c) { mock_ble_host_read_done(link, chr, BLE_ATT_ERR_UNLIKELY, NULL, 0); return; }
        NSNumber *k = @(chr);
        s_host->_reads[link][k] = @(s_host->_reads[link][k].intValue + 1);
        [s_host->_link[link] readValueForCharacteristic:c];
    });
}

void mock_ble_host_write(int link, int chr, const uint8_t *data, size_t len, bool response)
{
    NSData *d = [NSData dataWithBytes:data length:len];
    dispatch_async(s_host->_q, ^{
        CBCharacteristic *c = chr_at(link, chr);
        if (!c) { if (response) mock_ble_host_write_done(link, chr, BLE_ATT_ERR_UNLIKELY); return; }
        [s_host->_link[link] writeValue:d forCharacteristic:c
                                   type:response ? CBCharacteristicWriteWithResponse : CBCharacteristicWriteWithoutResponse];
    });
}

void mock_ble_host_subscribe(int link, int chr, bool on)
{
    dispatch_async(s_host->_q, ^{
        CBCharacteristic *c = chr_at(link, chr);
        if (!c) { mock_ble_host_subscribe_done(link, chr, BLE_ATT_ERR_UNLIKELY); return; }
        [s_host->_link[link] setNotifyValue:on forCharacteristic:c];
    });
}
