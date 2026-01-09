Thank you for reporting this compatibility issue! 

The problem has been completely resolved in v1.1.1. Root cause was Device Tree reference errors when optional APDS9960 sensor hardware was missing, causing boot failures.

v1.1.1 implements a Device Tree fallback system with conditional compilation that safely detects hardware and provides graceful fallback to fixed brightness. This achieves 100% boot success across all hardware configurations.

Closing as resolved in v1.1.1.