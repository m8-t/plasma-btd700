.pragma library

const names = {
    "sbc": "SBC",
    "aptx": "aptX",
    "aptx-adaptive": "aptX Adaptive",
    "aptx-lossless": "aptX Lossless",
    "aptx-lite": "aptX Lite",
    "lc3": "LC3"
};

function displayName(token) {
    return names[token] !== undefined ? names[token] : token;
}
