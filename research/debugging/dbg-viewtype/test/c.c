#include <stdio.h>

unsigned Ciallo = 0;

typedef struct T {
  unsigned field1;
  double field2[2];
  struct T* field3;
} T ;
T t[128];

typedef struct TT {
  T t;
  struct {
    signed a[1];
  };
  struct {
    struct {
      signed b;
    };
  };
  union { int c, d; };
  unsigned id;
} TT;
TT tt;

int main() {
	printf("%x\n", &Ciallo);
	printf("%x\n", &t[0].field1);
	printf("%x\n", &t[126].field2);
	printf("%x\n", t[0].field3);
	printf("tt %x\n", &tt);

}


