#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int running = 1;

static void *loop(void *a)
{
    (void)a;
    while (running)
        virEventRunDefaultImpl();
    return NULL;
}

static void cb(virConnectPtr c, virDomainPtr d, int ev, int det, void *o)
{
    (void)c; (void)d; (void)ev; (void)det; (void)o;
}

int main(int argc, char **argv)
{
    int n = argc > 2 ? atoi(argv[2]) : 30;
    pthread_t th;
    int i, bad = 0;

    virEventRegisterDefaultImpl();
    pthread_create(&th, NULL, loop, NULL);
    srand(7);

    for (i = 0; i < n; i++) {
        virConnectPtr c = virConnectOpen(argv[1]);
        int id;

        if (!c) { printf("run %d: open failed\n", i); bad++; continue; }
        id = virConnectDomainEventRegisterAny(c, NULL, VIR_DOMAIN_EVENT_ID_LIFECYCLE,
                                              VIR_DOMAIN_EVENT_CALLBACK(cb), NULL, NULL);
        if (id < 0) { printf("run %d: register failed\n", i); bad++; virConnectClose(c); continue; }
        /* land at different points within a poll cycle */
        usleep(900000 + (rand() % 1400000));
        if (i % 2)
            virConnectDomainEventDeregisterAny(c, id);
        virConnectClose(c);
    }
    running = 0;
    printf("done: %d runs, %d problems\n", n, bad);
    return bad != 0;
}
