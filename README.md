# speedtest-nRF91
Measures upload &amp; download bandwidth on the nRF9160-DK from Nordic Semiconductor using speedtest.net

This is a program that runs on the [nRF9160](https://www.nordicsemi.com/Products/Low-power-cellular-IoT/nRF9160) SiP (System-in-Package) from [Nordic Semiconductor](https://www.nordicsemi.com/).  It measures the realized upload and download network bandwidth by connecting to one of the worldwide network of servers provided by [speedtest.net](https://www.speedtest.net/).  The program automatically selects the most [geographically proximate server](https://help.speedtest.net/hc/en-us/articles/360039164573-Why-does-Speedtest-show-the-wrong-location-) based on the assigned IP address of the device.  Then it runs a download and upload test to measure the bandwidth usage and prints the results on the terminal screen.

As tested:
|  |  |
|--|--|
|**Hardware**| *nRF9160 DK v0.15.0 & v0.8.5* |
|**Modem Firmware**|*v1.2.2*  |
|**NCS SDK Tag**|*v1.4.2*  |
|**Toolchain**|*GNU Tools for Arm v9.2.1 20191025*  |
| | |


## Usage
 - Check out/extract the code in any directory.
 - Compile with command: 
 `west build -b nrf9160dk_nrf9160ns -p`
 - Open terminal to the nRF9160 DK with settings 115200, 8, N, 1.
 - Erase board and flash: 
  `nrfjprog -e && west flash`
 - Observe output on terminal:

    ![speedtest screenshot](https://github.com/r3b3lallianc3/speedtest-nRF91/blob/master/screenshot.png?raw=true)
 ## Tips / Info
  - The program connects to www.speedtest.net and downloads a list of servers that speedtest.net maintains.  Then the program parses this file to calculate the nearest server to connect to.  On subsequent runs, this list of servers is cached.  To have the program refresh the list of servers and not use the cached list from a previous run, press Button 1 upon boot and LED1 will light up to indicate that the program will refresh the list of servers.
  - The program does not use latency to determine the nearest server but rather the IP address.  This can be misleading if the IP address's location is incorrect.  See [here](https://help.speedtest.net/hc/en-us/articles/360039164573-Why-does-Speedtest-show-the-wrong-location-).  Using latency to calculate the optimal server for the speed test is yet to be implemented.
  - Sometimes the chosen server is down.  In that case, delete the cached list of servers, and try again.  The list of servers seems to be refreshed often.  One can also change locations to connect to a different cellular tower.
  - The certificate files for enabling HTTPS access to speednet.net servers are located in the cert/ directory.
  - The download_client_speedtest library is based on the [download_client](https://github.com/nrfconnect/sdk-nrf/tree/master/subsys/net/lib/download_client) library available with nRF Connect SDK. This version has been modified to accept multiple security certificates.
  - The upload_client library is based on the download_client_speedtest library with no analogs in the nRF Connect SDK.  There are some dependencies between these two libraries that need to be decoupled in the future.
  