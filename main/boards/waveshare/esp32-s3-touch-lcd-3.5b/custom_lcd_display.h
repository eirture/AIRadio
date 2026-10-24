#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include "lcd_display.h"

// // SPI LCD显示器
class CustomLcdDisplay : public LcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
    void SetupUI() override;
    void SetChatMessage(const char* role, const char* content) override;
    void UpdateIntercom() override;
private:
    lv_obj_t* intercom_bar_ = nullptr;
    lv_obj_t* intercom_button_ = nullptr;
    lv_obj_t* intercom_cancel_ = nullptr;
    lv_obj_t* intercom_label_ = nullptr;
    lv_obj_t* device_status_label_ = nullptr;
    lv_obj_t* intercom_status_label_ = nullptr;
    lv_obj_t* assistant_message_label_ = nullptr;
    static bool lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
    static void lvgl_port_flush_callback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map);
};



#endif // __CUSTOM_LCD_DISPLAY_H__
