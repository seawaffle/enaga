// Key test app: logs every key event as "K<key> <1 down | 0 up>", pad notes as "P<pad> <velocity | 0 release>",
// pad pressure as "A<pad> <0-127>" and knobs as "E<encoder> <detents>".
#include <enaga/app.hpp>

class Keys : public enaga::App {
public:
    void on_key(int key, bool down) override {
        char text[8] = {'K'};
        int n = 1;
        if (key >= 10) text[n++] = char('0' + key / 10);
        text[n++] = char('0' + key % 10);
        text[n++] = ' ';
        text[n++] = down ? '1' : '0';
        text[n] = 0;
        enaga::log(text);
    }
    void on_pad_press(int pad, int velocity) override { pad_line(pad, velocity); }
    void on_pad_release(int pad) override { pad_line(pad, 0); }
    void on_pad_pressure(int pad, int pressure) override { pad_line(pad, pressure, 'A'); }
    void on_encoder(int encoder, int delta) override {
        char text[12] = {'E', char('0' + encoder), ' '};
        int n = 3;
        if (delta < 0) { text[n++] = '-'; delta = -delta; }
        if (delta >= 100) text[n++] = char('0' + delta / 100 % 10);
        if (delta >= 10) text[n++] = char('0' + delta / 10 % 10);
        text[n++] = char('0' + delta % 10);
        text[n] = 0;
        enaga::log(text);
    }

private:
    static void pad_line(int pad, int value, char kind = 'P') {
        char text[12] = {kind};
        int n = 1;
        if (pad >= 10) text[n++] = char('0' + pad / 10);
        text[n++] = char('0' + pad % 10);
        text[n++] = ' ';
        if (value >= 100) text[n++] = char('0' + value / 100);
        if (value >= 10) text[n++] = char('0' + value / 10 % 10);
        text[n++] = char('0' + value % 10);
        text[n] = 0;
        enaga::log(text);
    }
};

ENAGA_APP(Keys)
