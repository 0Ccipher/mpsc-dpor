#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>


atomic_int x;
atomic_int y;

void *thread_1(void *unused)
{
	atomic_store_explicit(&x, 1, memory_order_release);
	atomic_store_explicit(&y, 2, memory_order_release);
	return NULL;
}

void *thread_2(void *unused)
{
	atomic_store_explicit(&y, 1, memory_order_release);
	atomic_store_explicit(&x, 2, memory_order_release);
	return NULL;
}



int main()
{
	pthread_t t1, t2;

	if (pthread_create(&t1, NULL, thread_1, NULL))
		abort();
	if (pthread_create(&t2, NULL, thread_2, NULL))
		abort();

	return 0;
}
