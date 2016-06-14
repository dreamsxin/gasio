
#ifdef __APPLE__


#include <errno.h>			// errno
#include <stdlib.h>			// calloc, free
#include <string.h>			// strerror
#ifdef __linux__
#include <signal.h>
#endif

#include "GASsupport.h"
#include "GAStasks.h"
#include "GASsockets.h"
#include "GASthreads.h"
#include "GASkqueue.h"


// agnostic

GAS_POLLER_INFO *
gas_create_poller (void* parent, char* address, int port, char *networks, void (*callback)(gas_client_info *ci,int op))
{
	gas_socket_t server_socket;
	if ((server_socket = gas_create_server_socket (address, port)) < 0)
		return NULL;

	GAS_POLLER_INFO *pi = calloc (1, sizeof(GAS_POLLER_INFO));
	pi->parent = parent;
	pi->info_type = GAS_TP_POLLER;
	pi->allow_tasks = GAS_TRUE;
	pi->clients_are_non_blocking = GAS_TRUE;
	gas_preset_client_config (pi);
	pi->server_socket = server_socket;
	pi->actual_connections = 0;
	gas_assign_networks ((void *)pi, networks);
	pi->callback = callback;
	pi->stop = GAS_FALSE;
	gas_init_clients (pi);
	gas_init_tasks();

	return pi;
}

int
gas_start_poller (GAS_POLLER_INFO *pi)
{
	pi->accept_ci = gas_create_client (pi, pi->server_socket);

	if ((pi->poll_handler = gas_create_poll()) < 0) {
		pi->server_socket = gas_close_socket (pi->server_socket);
		gas_error_message ("Error creating poll queue: %s\n", strerror (errno));
		return GAS_FALSE;
	}

	return gas_create_thread (&pi->poll_thread, gas_event_loop, pi);
}

void *
gas_stop_poller (GAS_POLLER_INFO *pi)
{
	pi->stop = GAS_TRUE;

	pi->server_socket = gas_close_socket (pi->server_socket);
	pi->accept_ci = gas_delete_client (pi->accept_ci, GAS_FALSE);

	gas_client_info *ci = pi->first_client;
	while (ci != NULL) {
		gas_client_info *next = ci->next;
		gas_close_socket (ci->socket);
		gas_delete_client (ci, GAS_TRUE);
		ci = next;
	}

	pi->poll_handler = gas_close_socket (pi->poll_handler);

	if (pi->poll_thread != 0) {
#ifdef __linux__
		pthread_kill(pi->poll_thread, SIGHUP);
#endif // __linux__		gas_wait_thread (pi->poll_thread);
		gas_wait_thread (pi->poll_thread);
		pi->poll_thread = 0;
	}

	gas_clean_tasks();
	gas_clean_clients (pi);
	gas_release_networks (pi);

	free (pi);
	return NULL;
}

int
gas_poller_is_alive (GAS_POLLER_INFO *pi)
{
	if (pi->info_type == GAS_TP_POLLER)
		return pi->poll_thread != 0;
	else
		return 0;
}


#ifdef __linux__
void
poller_signal (int sig, siginfo_t *info, void *ucontext)
{
}
#endif

__stdcall gas_callback_t
gas_event_loop (void *tpi)
{
	GAS_POLLER_INFO *pi = (GAS_POLLER_INFO *)tpi;
	gas_poll_event *events_queue = NULL;
	int events_queue_size = 0;

#ifdef __linux__
	// to awake accept if linux
	struct sigaction sa;
	sa.sa_handler = NULL;
	sa.sa_sigaction = poller_signal;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGHUP, &sa, NULL) < 0) {
		gas_error_message("error setting sigaction");
		return NULL;
	}
#endif

	gas_add_server_to_poll (pi->accept_ci);

	while (!pi->stop) {
		int old_events_queue_size = events_queue_size;
		if (events_queue_size==0 || events_queue_size<pi->actual_connections)
			events_queue_size += GAS_EVENTS_QUEUE_SIZE;								// grow
		if (events_queue_size>pi->actual_connections*2 && events_queue_size>GAS_EVENTS_QUEUE_SIZE)
			events_queue_size += GAS_EVENTS_QUEUE_SIZE;								// shrink
		if (old_events_queue_size != events_queue_size)  {
			if (events_queue != NULL)
				free(events_queue);
			events_queue = (gas_poll_event *) calloc (events_queue_size, sizeof(gas_poll_event));
		}

		int nevents = gas_wait_event (pi, events_queue, events_queue_size);
		gas_poll_event *pev;
		gas_debug_message (GAS_IO, "nevents=%d\n", nevents);

		if (nevents < 0) {
			if (!pi->stop)
				gas_error_message ("Error in waiting events: %s\n", strerror (errno));
			break;
		}
		if (nevents == 0) {
			gas_error_message ("No events received!\n");
			break;
		}

		int ievent;
		for (ievent = 0; ievent < nevents; ievent++) {
			pev = &events_queue[ievent];
			gas_client_info *ci = (gas_client_info *) GAS_DATA(pev);
			if (ci==NULL)
				break;
			gas_debug_message (GAS_IO, "event socket=%d, ci=%6x, ev=%s, op=%s, flags=%s%s\n",
					ci->socket, ci, gas_name_event(GAS_EV(pev)), gas_name_operation(GAS_OP(pev)),
					(pev->flags&EV_DELETE)?"DELETE ":"", (pev->flags&EV_ERROR)?"ERROR ":"");
			if (GAS_IGNORE_EV(pev))
				continue;
			if (GAS_FALSE_EOF_EV(pev))
				ci->read_end = GAS_TRUE;
			else if (ci == pi->accept_ci)
				gas_do_accept (pi, ci->socket);
			else {
				if (GAS_READ_EV(pev)) {
					if (gas_do_read (ci) > 0)
						gas_do_callback (ci, GAS_CLIENT_READ);
				}
				if (GAS_WRITE_EV(pev)) {		// if use_write_events. in linux, no write event until queue full
					if (ci->write_pending)
						gas_do_write (ci);
					else
						ci->can_write = GAS_TRUE;
				}
				while (ci->callback_has_written && ci->can_write)
					if (ci->can_write && ci->step >= 0)
						gas_do_callback (ci, GAS_CLIENT_WRITE);
			}
			if (ci->error || ((ci->read_end && GAS_CI_DATA_SIZE(ci->rb)==0) /*&& GAS_CI_DATA_SIZE(ci->wb)==0*/)) {
				gas_debug_message (GAS_IO, "close socket=%d, ci=%6x, ev=%s, op=%s\n",
						ci->socket, ci, gas_name_event(GAS_EV(pev)), gas_name_operation(GAS_OP(pev)));
				gas_remove_client_from_poll (ci);
				if (GAS_TRUE_EOF_EV(pev)) {
					ci->socket = gas_close_socket (ci->socket);
					ci = gas_delete_client (ci, GAS_TRUE);
				}
			}
		}
	}

	if (events_queue != NULL)
		free(events_queue);
	pi->poll_thread = 0;			// signal end of thread
	return (gas_callback_t) 0;
}


// specific functions for kqueue

int gas_create_poll () {
	return kqueue ();
}

int gas_wait_event (GAS_POLLER_INFO *pi, gas_poll_event *events_queue, int events_queue_size) {
	return kevent (pi->poll_handler, NULL, 0, events_queue, events_queue_size, NULL);
}

void gas_add_server_to_poll (gas_client_info *ci) {
	gas_change_poll (ci, EV_ADD | EV_ENABLE, EVFILT_READ);
}

void gas_add_client_to_poll (gas_client_info *ci) {
	((GAS_POLLER_INFO *)ci->tpi)->actual_connections++;
	gas_change_poll (ci, EV_ADD|EV_ENABLE|EV_CLEAR, EVFILT_READ);
	if (ci->use_write_events)
		gas_change_poll (ci, EV_ADD|EV_ENABLE|EV_CLEAR, EVFILT_WRITE);
}

void gas_remove_client_from_poll (gas_client_info *ci) {
	((GAS_POLLER_INFO *)ci->tpi)->actual_connections--;
	gas_change_poll (ci, EV_DELETE, EVFILT_READ);
	if (ci->use_write_events)
		gas_change_poll (ci, EV_DELETE, EVFILT_WRITE);
}

void
gas_change_poll (gas_client_info *ci, int operation, int events)
{
	gas_poll_event pe;
	pe.ident  = ci->socket;
	pe.filter = events;
	pe.flags  = operation;
	pe.fflags = 0;
	pe.data   = 0;
	pe.udata  = ci;
  	gas_debug_message (GAS_IO, "gas_change_poll socket=%d, ci=%6x, ev=%s, op=%s\n",
  		ci->socket, ci, gas_name_event(GAS_EV((&pe))), gas_name_operation(GAS_OP((&pe))));
	int nevents = kevent (((GAS_POLLER_INFO *)ci->tpi)->poll_handler, &pe, 1, NULL, 0, NULL);
	if (nevents < 0)
		gas_error_message ("Error in gas_change_poll: %s\n", strerror (errno));
}

const char *
gas_name_event (int event)
{
	switch (event) {
	case EVFILT_READ:				// -1
		return "EVFILT_READ ";
	case EVFILT_WRITE:				// -2
		return "EVFILT_WRITE";
	default:
		return "EVFILT_???  ";
	}
}

const char *gas_name_operation (int operation)
{
	static char string[51];
	char *sep="", *ps=string;
	int bit=0x0001;
	do {
		switch (operation&bit) {
		case EV_ADD:		// 0001
			strcpy(ps,sep); ps+=strlen(sep); strcpy(ps,"EV_ADD");     ps+=6; break;
		case EV_DELETE:		// 0002
			strcpy(ps,sep); ps+=strlen(sep); strcpy(ps,"EV_DELETE");  ps+=9; break;
		case EV_ENABLE:		// 0004
			strcpy(ps,sep); ps+=strlen(sep); strcpy(ps,"EV_ENABLE");  ps+=9; break;
		case EV_DISABLE:	// 0008
			strcpy(ps,sep); ps+=strlen(sep); strcpy(ps,"EV_DISABLE"); ps+=10; break;
		case EV_CLEAR:		// 0020
			strcpy(ps,sep); ps+=strlen(sep); strcpy(ps,"EV_CLEAR");   ps+=8; break;
		case EV_EOF:		// 8000
			strcpy(ps,sep); ps+=strlen(sep); strcpy(ps,"EV_EOF");     ps+=6; break;
		case EV_ERROR:		// 4000
			strcpy(ps,sep); ps+=strlen(sep); strcpy(ps,"EV_ERROR");   ps+=8; break;
		default:
			break;
		}
		if (ps!=string)
			sep = "|";
		bit <<= 1;
		if (ps-string >= 40)
			break;
	} while (bit < 0x10000);
	*ps = '\0';
	return string;
}


#endif // __APPLE__
