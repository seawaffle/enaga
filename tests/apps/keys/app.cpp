// Key test app: logs every key event as "K<key> <1 down | 0 up>", and pad notes as "P<pad> <velocity | 0 release>".
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

private:
    static void pad_line(int pad, int value) {
        char text[12] = {'P'};
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
