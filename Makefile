
.PHONY: clean
clean:
	touch ./debug/safe.txt; rm ./debug/*.txt ; make compile

compile: codice_multilivello.c
	gcc -Wall -pedantic codice_multilivello.c -O2  -o codice  -Wall -pedantic ;
	
compile_serial: codice_multilivello.c
	gcc -Wall -pedantic codice_multilivello.c -g  -o codice  -Wall -pedantic  -D SERIAL;

serial_run: codice_multilivello.c
	make; make compile_serial; ./codice $(pages);
	
parallel_run: codice_multilivello.c
	make; ./codice $(proc) $(pages); 
	
serial_run_open: codice_multilivello.c
	make serial_run; open ./debug/*.txt;
	
parallel_run_open: codice_multilivello.c
	make parallel_run; open ./debug/*.txt;
	
valgrind: codice_multilivello.c
	make ; valgrind ./codice $(proc) $(pages)

numa: codice_multilivello.c
	gcc -Wall  ./codice_multilivello.c -g -o codice -lnuma -D NUMA;
	
numa_audit: codice_multilivello.c
	gcc -Wall  ./codice_multilivello.c -g -o codice -lnuma -D NUMA -D NUMA_AUDIT;
	
numa_valgrind: codice_multilivello.c
	gcc -Wall  ./codice_multilivello.c -g -o codice -lnuma -D NUMA -D NUMA_AUDIT; 
	valgrind ./codice $(proc) $(pages) 2> out.txt; 
	
make numa_run_and_print: codice_multilivello.c
	gcc -Wall  ./codice_multilivello.c -g -o codice -lnuma -D NUMA -D NUMA_AUDIT; 
	./codice $(proc) $(pages) > out.txt; 