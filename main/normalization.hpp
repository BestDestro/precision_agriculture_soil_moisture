#pragma once

// Orden exacto del modelo:
// [128mhz, 64mhz, 32mhz, 16mhz, 8mhz, 4mhz, 2mhz, 1mhz, 500khz, std_128mhz, std_64mhz, std_32mhz, std_16mhz, std_8mhz, std_4mhz, std_2mhz, std_1mhz, std_500khz]

static constexpr int kInputSize = 18;

static constexpr float kXMean[kInputSize] = {
    0.50671434f,
    0.51105064f,
    0.51353824f,
    0.51462698f,
    0.51607770f,
    0.51679677f,
    0.51620960f,
    0.51766944f,
    0.51893026f,
    0.05683157f,
    0.05185148f,
    0.04949703f,
    0.04928500f,
    0.04742135f,
    0.04775856f,
    0.04684879f,
    0.04575821f,
    0.04681152f
};

static constexpr float kXStd[kInputSize] = {
    0.25252816f,
    0.25302443f,
    0.25283793f,
    0.25060174f,
    0.25002000f,
    0.25008443f,
    0.24882358f,
    0.25155869f,
    0.25184056f,
    0.05941683f,
    0.05469417f,
    0.05857234f,
    0.06013905f,
    0.05831286f,
    0.05773451f,
    0.05409672f,
    0.04986083f,
    0.05118759f
};

static constexpr float kYMean = 18.11347961f;
static constexpr float kYStd  = 10.86786747f;
