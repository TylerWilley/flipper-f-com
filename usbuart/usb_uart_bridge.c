#include "usb_uart_bridge.h"
#include "usb_cdc.h"
#include <cli/cli_vcp.h>
#include <toolbox/api_lock.h>
#include <furi_hal.h>
#include <furi_hal_usb_cdc.h>

//TODO: FL-3276 port to new USART API
#include <stm32wbxx_ll_lpuart.h>
#include <stm32wbxx_ll_usart.h>

#define USB_CDC_PKT_LEN CDC_DATA_SZ
#define USB_UART_RX_BUF_SIZE (USB_CDC_PKT_LEN * 5)

#define USB_CDC_BIT_DTR (1 << 0)
#define USB_CDC_BIT_RTS (1 << 1)
#define USB_USART_DE_RE_PIN &gpio_ext_pa4

typedef enum {
    WorkerEvtStop = (1 << 0),
    WorkerEvtRxDone = (1 << 1),

    WorkerEvtTxStop = (1 << 2),
    WorkerEvtCdcRx = (1 << 3),
    WorkerEvtCdcTxComplete = (1 << 4),

    WorkerEvtCfgChange = (1 << 5),

    WorkerEvtLineCfgSet = (1 << 6),
    WorkerEvtCtrlLineSet = (1 << 7),

} WorkerEvtFlags;

#define WORKER_ALL_RX_EVENTS                                                      \
    (WorkerEvtStop | WorkerEvtRxDone | WorkerEvtCfgChange | WorkerEvtLineCfgSet | \
     WorkerEvtCtrlLineSet | WorkerEvtCdcTxComplete)
#define WORKER_ALL_TX_EVENTS (WorkerEvtTxStop | WorkerEvtCdcRx)

struct UsbUartBridge {
    UsbUartConfig cfg;
    UsbUartConfig cfg_new;

    FuriThread* thread;
    FuriThread* tx_thread;

    FuriStreamBuffer* rx_stream;

    FuriMutex* usb_mutex;

    FuriSemaphore* tx_sem;

    UsbUartState st;

    FuriApiLock cfg_lock;

    uint8_t rx_buf[USB_CDC_PKT_LEN];
};

static void vcp_on_cdc_tx_complete(void* context);
static void vcp_on_cdc_rx(void* context);
static void vcp_state_callback(void* context, uint8_t state);
static void vcp_on_cdc_control_line(void* context, uint8_t state);
static void vcp_on_line_config(void* context, struct usb_cdc_line_coding* config);

static const CdcCallbacks cdc_cb = {
    vcp_on_cdc_tx_complete,
    vcp_on_cdc_rx,
    vcp_state_callback,
    vcp_on_cdc_control_line,
    vcp_on_line_config,
};

/* USB UART worker */

static int32_t usb_uart_tx_thread(void* context);

void usb_uart_send(UsbUartBridge* usb_uart, const uint8_t* data, size_t len) {
    furi_stream_buffer_send(usb_uart->rx_stream, data, len, 0);
    furi_thread_flags_set(furi_thread_get_id(usb_uart->thread), WorkerEvtRxDone);
}

/* Copied from furi_hal_usb_cdc.c so we can change our vid/pid for alpha terminal/serial */

#define USB_EP0_SIZE 8

enum UsbDevDescStr {
    UsbDevLang = 0,
    UsbDevManuf = 1,
    UsbDevProduct = 2,
    UsbDevSerial = 3,
};

static const struct usb_device_descriptor cdc_device_desc_fcom = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = VERSION_BCD(2, 0, 0),
    .bDeviceClass = USB_CLASS_IAD,
    .bDeviceSubClass = USB_SUBCLASS_IAD,
    .bDeviceProtocol = USB_PROTO_IAD,
    .bMaxPacketSize0 = USB_EP0_SIZE,
    .idVendor = 0x2341,
    .idProduct = 0x0000,
    .bcdDevice = VERSION_BCD(1, 0, 0),
    .iManufacturer = UsbDevManuf,
    .iProduct = UsbDevProduct,
    .iSerialNumber = UsbDevSerial,
    .bNumConfigurations = 1,
};

static const struct usb_string_descriptor dev_manuf_desc = USB_STRING_DESC("Flipper Devices Inc.");

FuriHalUsbInterface usb_cdc_fcom = {
    .dev_descr = (struct usb_device_descriptor*)&cdc_device_desc_fcom,
    .str_manuf_descr = (void*)&dev_manuf_desc,
    .str_prod_descr = NULL,
    .str_serial_descr = NULL,
};

static void usb_uart_vcp_init(UsbUartBridge* usb_uart, uint8_t vcp_ch) {
    furi_hal_usb_unlock();
    if(vcp_ch == 0) {
        CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
        cli_vcp_disable(cli_vcp);
        furi_record_close(RECORD_CLI_VCP);
        furi_check(furi_hal_usb_set_config(&usb_cdc_fcom, NULL) == true);
    } else {
        furi_check(furi_hal_usb_set_config(&usb_cdc_dual, NULL) == true);
        CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
        cli_vcp_enable(cli_vcp);
        furi_record_close(RECORD_CLI_VCP);
    }
    furi_hal_cdc_set_callbacks(vcp_ch, (CdcCallbacks*)&cdc_cb, usb_uart);
}

static void usb_uart_vcp_deinit(UsbUartBridge* usb_uart, uint8_t vcp_ch) {
    UNUSED(usb_uart);
    furi_hal_cdc_set_callbacks(vcp_ch, NULL, NULL);
    if(vcp_ch != 0) {
        CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
        cli_vcp_disable(cli_vcp);
        furi_record_close(RECORD_CLI_VCP);
    }
}

static int32_t usb_uart_worker(void* context) {
    UsbUartBridge* usb_uart = (UsbUartBridge*)context;

    memcpy(&usb_uart->cfg, &usb_uart->cfg_new, sizeof(UsbUartConfig));

    usb_uart->rx_stream = furi_stream_buffer_alloc(USB_UART_RX_BUF_SIZE, 1);

    usb_uart->tx_sem = furi_semaphore_alloc(1, 1);
    usb_uart->usb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    usb_uart->tx_thread =
        furi_thread_alloc_ex("UsbUartTxWorker", 512, usb_uart_tx_thread, usb_uart);

    usb_uart_vcp_init(usb_uart, usb_uart->cfg.vcp_ch);

    furi_thread_flags_set(furi_thread_get_id(usb_uart->tx_thread), WorkerEvtCdcRx);

    furi_thread_start(usb_uart->tx_thread);

    while(1) {
        uint32_t events =
            furi_thread_flags_wait(WORKER_ALL_RX_EVENTS, FuriFlagWaitAny, FuriWaitForever);
        furi_check(!(events & FuriFlagError));
        if(events & WorkerEvtStop) break;
        if(events & (WorkerEvtRxDone | WorkerEvtCdcTxComplete)) {
            size_t len = furi_stream_buffer_receive(
                usb_uart->rx_stream, usb_uart->rx_buf, USB_CDC_PKT_LEN, 0);
            if(len > 0) {
                if(furi_semaphore_acquire(usb_uart->tx_sem, 100) == FuriStatusOk) {
                    usb_uart->st.rx_cnt += len;
                    furi_check(
                        furi_mutex_acquire(usb_uart->usb_mutex, FuriWaitForever) == FuriStatusOk);
                    furi_hal_cdc_send(usb_uart->cfg.vcp_ch, usb_uart->rx_buf, len);
                    furi_check(furi_mutex_release(usb_uart->usb_mutex) == FuriStatusOk);
                } else {
                    furi_stream_buffer_reset(usb_uart->rx_stream);
                }
            }
        }
        if(events & WorkerEvtCfgChange) {
            if(usb_uart->cfg.vcp_ch != usb_uart->cfg_new.vcp_ch) {
                furi_thread_flags_set(furi_thread_get_id(usb_uart->tx_thread), WorkerEvtTxStop);
                furi_thread_join(usb_uart->tx_thread);

                usb_uart_vcp_deinit(usb_uart, usb_uart->cfg.vcp_ch);
                usb_uart_vcp_init(usb_uart, usb_uart->cfg_new.vcp_ch);

                usb_uart->cfg.vcp_ch = usb_uart->cfg_new.vcp_ch;
                furi_thread_start(usb_uart->tx_thread);
                events |= WorkerEvtCtrlLineSet;
                events |= WorkerEvtLineCfgSet;
            }
            api_lock_unlock(usb_uart->cfg_lock);
        }
    }
    usb_uart_vcp_deinit(usb_uart, usb_uart->cfg.vcp_ch);

    //furi_hal_gpio_init(USB_USART_DE_RE_PIN, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

    furi_thread_flags_set(furi_thread_get_id(usb_uart->tx_thread), WorkerEvtTxStop);
    furi_thread_join(usb_uart->tx_thread);
    furi_thread_free(usb_uart->tx_thread);

    furi_stream_buffer_free(usb_uart->rx_stream);
    furi_mutex_free(usb_uart->usb_mutex);
    furi_semaphore_free(usb_uart->tx_sem);

    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_cdc_single, NULL) == true);
    CliVcp* cli_vcp = furi_record_open(RECORD_CLI_VCP);
    cli_vcp_enable(cli_vcp);
    furi_record_close(RECORD_CLI_VCP);

    return 0;
}

static int32_t usb_uart_tx_thread(void* context) {
    UsbUartBridge* usb_uart = (UsbUartBridge*)context;

    uint8_t data[USB_CDC_PKT_LEN];
    while(1) {
        uint32_t events =
            furi_thread_flags_wait(WORKER_ALL_TX_EVENTS, FuriFlagWaitAny, FuriWaitForever);
        furi_check(!(events & FuriFlagError));
        if(events & WorkerEvtTxStop) break;
        if(events & WorkerEvtCdcRx) {
            furi_check(furi_mutex_acquire(usb_uart->usb_mutex, FuriWaitForever) == FuriStatusOk);
            size_t len = furi_hal_cdc_receive(usb_uart->cfg.vcp_ch, data, USB_CDC_PKT_LEN);
            furi_check(furi_mutex_release(usb_uart->usb_mutex) == FuriStatusOk);

            if(len > 0) {
                usb_uart->st.tx_cnt += len;

                usb_uart->cfg.cb(usb_uart->cfg.ctx, data, len);
            }
        }
    }
    return 0;
}

/* VCP callbacks */

static void vcp_on_cdc_tx_complete(void* context) {
    UsbUartBridge* usb_uart = (UsbUartBridge*)context;
    furi_semaphore_release(usb_uart->tx_sem);
    furi_thread_flags_set(furi_thread_get_id(usb_uart->thread), WorkerEvtCdcTxComplete);
}

static void vcp_on_cdc_rx(void* context) {
    UsbUartBridge* usb_uart = (UsbUartBridge*)context;
    furi_thread_flags_set(furi_thread_get_id(usb_uart->tx_thread), WorkerEvtCdcRx);
}

static void vcp_state_callback(void* context, uint8_t state) {
    UNUSED(context);
    UNUSED(state);
    // Don't care
}

static void vcp_on_cdc_control_line(void* context, uint8_t state) {
    UNUSED(state);
    UsbUartBridge* usb_uart = (UsbUartBridge*)context;
    furi_thread_flags_set(furi_thread_get_id(usb_uart->thread), WorkerEvtCtrlLineSet);
}

static void vcp_on_line_config(void* context, struct usb_cdc_line_coding* config) {
    UNUSED(config);
    UsbUartBridge* usb_uart = (UsbUartBridge*)context;
    furi_thread_flags_set(furi_thread_get_id(usb_uart->thread), WorkerEvtLineCfgSet);
}

UsbUartBridge* usb_uart_enable(UsbUartConfig* cfg) {
    /*
    NOTE: We need to replace the vid/pid so alpha terminal/serial android
    detect this as an arduino... But we can't reference the cdc_init
    method 'cause it's in the firmware's private code. So overwrite
    our handles here instead.
    */
    usb_cdc_fcom.init = usb_cdc_single.init;
    usb_cdc_fcom.deinit = usb_cdc_single.deinit;
    usb_cdc_fcom.wakeup = usb_cdc_single.wakeup;
    usb_cdc_fcom.suspend = usb_cdc_single.suspend;
    usb_cdc_fcom.cfg_descr = usb_cdc_single.cfg_descr;

    UsbUartBridge* usb_uart = malloc(sizeof(UsbUartBridge));

    memcpy(&(usb_uart->cfg_new), cfg, sizeof(UsbUartConfig));

    usb_uart->thread = furi_thread_alloc_ex("UsbUartWorker", 1024, usb_uart_worker, usb_uart);

    furi_thread_start(usb_uart->thread);
    return usb_uart;
}

void usb_uart_disable(UsbUartBridge* usb_uart) {
    furi_assert(usb_uart);
    furi_thread_flags_set(furi_thread_get_id(usb_uart->thread), WorkerEvtStop);
    furi_thread_join(usb_uart->thread);
    furi_thread_free(usb_uart->thread);
    free(usb_uart);
}

void usb_uart_set_config(UsbUartBridge* usb_uart, UsbUartConfig* cfg) {
    furi_assert(usb_uart);
    furi_assert(cfg);
    usb_uart->cfg_lock = api_lock_alloc_locked();
    memcpy(&(usb_uart->cfg_new), cfg, sizeof(UsbUartConfig));
    furi_thread_flags_set(furi_thread_get_id(usb_uart->thread), WorkerEvtCfgChange);
    api_lock_wait_unlock_and_free(usb_uart->cfg_lock);
}

void usb_uart_get_config(UsbUartBridge* usb_uart, UsbUartConfig* cfg) {
    furi_assert(usb_uart);
    furi_assert(cfg);
    memcpy(cfg, &(usb_uart->cfg_new), sizeof(UsbUartConfig));
}

void usb_uart_get_state(UsbUartBridge* usb_uart, UsbUartState* st) {
    furi_assert(usb_uart);
    furi_assert(st);
    memcpy(st, &(usb_uart->st), sizeof(UsbUartState));
}
