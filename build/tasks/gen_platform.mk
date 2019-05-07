ifneq ($(filter salvator, $(TARGET_DEVICE)),)

$(PRODUCT_OUT)/platform.txt:
	echo $(TARGET_BOARD_PLATFORM) > $@

droidcore: $(PRODUCT_OUT)/platform.txt

endif
