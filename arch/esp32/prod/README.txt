#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
#

This directory contains scripts to make Ayla Host Application Modules
based on the ESP32 and ESP32-C3.

The scripts use other scripts supplied in the ESP-IDF.

To use these, extract the contents of the .zip archive containing this README.

Script setup
_________
1. Rename the directory containing this archive to prod.
The contents of prod should include this README and the other files
inside this directory:

	prod
	|-- esp-idf
	|   `-- components
	|       |-- esptool_py
	|       |   `-- esptool
	|       |       |-- CONTRIBUTING.md
	|       |       |-- espefuse.py
	|       |       |-- espressif
	|       |       ...
	|       `-- nvs_flash
	|           `-- nvs_partition_generator
	|               `-- nvs_partition_gen.py
	|-- ext
	|   `-- mfg_gen.py
	|-- prod_run.py
	|-- prod_tool.py
	|-- pyayla
	|   |-- ayla_conf_tokens.py
	|   |-- __init__.py
	|   |-- oem_config.py
	|   |-- prod_batch.py
	|   |-- prod_csv.py
	|   |-- prod_expect.py
	|   `-- prod_install.py
	`-- README.txt

2. Install any required python packages, if needed.
   Check the list in the ESP-IDF top-level file requirements.txt.

Batch Preparation
-----------------

1. Obtain a file of device serial numbers (DSNs) and keys from the
Ayla dashboard, under the "Factory Actions" tab.

The DSNs should be reserved under the module maker's Ayla
OEM ID, and have the part number assigned to the devices to be made.
For example, for LED bulb modules, the part number is AY028MLB1.
Other part numbers in the AY028xxx series should be used for these parts,
as directed.  Use the Reserve job, and request the number of DSNs you expect
to produce in this batch.

Once the reserve job is complete, download the .zip file produced and extract
the contents into a directory called 'dsns'.

2. Determine the serial ports to be used in programming the devices, perhaps
using the Windows Device Manager.  Put those device names, one per line,
in conf\com_ports.txt, using the Notepad app.

Flashing and verifying
______________________

3. Run the script in a command window. The commmand to run the script is:

	python prod/prod_run.py --model LA68701 \
		--app Ayla-esp32-starter_app-0.3-8M

Note, the model name should be replaced with the name of the module
being configured.  The --app argument should be replaced with the file name
for the application package being configured.

Help is available with the -h option.

A --serial argument can be supplied to give a starting base for the
serial numbers in the modules.  The script appends a sequence number to that.
The default is a timestamp of when this script is run for the batch.

The --port argument can be used to specify a single serial port to use instead
of the ones listed in conf\com_ports.txt.

4.  Before each group, the script prompts the user to enter 'c' before
continuing to program the next part, or 'q' to quit.

5. If everything works, the parts should be flashed.  There's currently no
progress report, and each group takes 30-55 seconds.

A more complete document is available separately.

--- end ---
