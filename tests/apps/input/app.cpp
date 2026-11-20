// Input test app: plays its input straight to the output, and chooses the input with the STEP keys:
// STEP 1-5 the five sources at NORMAL (built-in mic, Hi-Z, dynamic mic, stereo line, mono line), STEP 6 stereo line
// LOW (the PAD), STEP 7 Hi-Z HIGH, STEP 8 dynamic mic HIGH, STEP 9 an invalid source. Logs "I<result>".
#include <enaga/app.hpp>

class Input : public enaga::App {
public:
    void on_key(int key, bool down) override {
        static const int choices[][2] = {
            {ENAGA_INPUT_MIC, ENAGA_INPUT_NORMAL}, {ENAGA_INPUT_HIZ, ENAGA_INPUT_NORMAL},
            {ENAGA_INPUT_DYNAMIC_MIC, ENAGA_INPUT_NORMAL}, {ENAGA_INPUT_LINE_STEREO, ENAGA_INPUT_NORMAL},
            {ENAGA_INPUT_LINE_MONO, ENAGA_INPUT_NORMAL}, {ENAGA_INPUT_LINE_STEREO, ENAGA_INPUT_LOW},
            {ENAGA_INPUT_HIZ, ENAGA_INPUT_HIGH}, {ENAGA_INPUT_DYNAMIC_MIC, ENAGA_INPUT_HIGH}, {7, ENAGA_INPUT_NORMAL}};
        int n = key - ENAGA_KEY_STEP1;
        if (!down || n < 0 || n >= 9) return;
        int result = enaga::system::input(choices[n][0], choices[n][1]);
        char text[4] = {'I', result < 0 ? '-' : '0', result < 0 ? char('0' - result) : char(0), 0};
        enaga::log(text);
    }
    void audio(const float *in, float *out, uint32_t frames) override {
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = in[i];
    }
};

ENAGA_APP(Input)
