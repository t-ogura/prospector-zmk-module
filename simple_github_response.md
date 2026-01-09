Hello @Gasiro,

Thank you for the excellent bug report! This issue has been completely resolved in v1.1.1.

The root cause was Device Tree reference errors when optional sensor hardware was missing, causing boot failures. v1.1.1 implements a Device Tree fallback system that works with your original setup. The complete technical details are documented in Issue #2: https://github.com/t-ogura/zmk-config-prospector/issues/2

Thanks for your patience and feedback - it was instrumental in fixing this compatibility issue!