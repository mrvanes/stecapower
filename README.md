# stecapower
Litttle daemon to read power from digital powermeter

# Installation
Install wiringpi
```
$ sudo apt-get install wiringpi
```
Make stecapower
```
$ make
```
Install
```
$ mkdir -p /usr/local/sbin
$ cp stecapower /usr/local/sbin
```
Run
```
$ sudo /usr/local/sbin/stecapower
```

# Connection
Make sure to get a 0.1Hz/W (1 pulse/10W) signal on GPIO17 using e.g. a commercial off-the-shelf Power meter. Preferably via optocoupler.

# Usage
stecapower listens on port 54321 and outputs current power (W), accumulated energy (Wh) and total received pulses
```
$ nc localhost 54321
393 255 91880
```
To reset the counter, send SIGHUP to stecapower process (e.g. using daily cron)
```
$ sudo pkill -hup stecapower
```

stecapower saves it's state on shutdown, so you won't loose daily generation on restart.
