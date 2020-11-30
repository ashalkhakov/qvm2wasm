FILES=$(patsubst %.dot,%.ps,$(wildcard *.dot))

all: $(FILES)

%.ps: %.dot
	dot -Tps $< -o $@

clean::
	rm *.ps
