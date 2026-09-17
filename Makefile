all: driver_module umd_lib user_tests

driver_module:
	$(MAKE) -C driver/

umd_lib:
	$(MAKE) -C umd/

user_tests: umd_lib
	$(MAKE) -C tests/

clean:
	$(MAKE) -C driver/ clean
	$(MAKE) -C umd/ clean
	$(MAKE) -C tests/ clean

.PHONY: all driver_module umd_lib user_tests clean
