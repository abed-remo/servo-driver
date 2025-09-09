obj-m += servo.o

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) clean

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) modules_install
