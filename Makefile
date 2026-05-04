searx: src/searx.c
	$(CC) -O2 $< -lcurl -o $@

clean:
	rm -f searx
