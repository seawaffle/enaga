// Key test app: logs every key event as "K<key> <1 down | 0 up>".
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
};

ENAGA_APP(Keys)
