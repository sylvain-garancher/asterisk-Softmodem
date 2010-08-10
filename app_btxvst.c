/*
 * BTX-Vermittlungsstelle for Asterisk
 *
 * 2010, Christian Groeger <chris@proquari.at>
 * 
 * Based on app_fax.c by Dmitry Andrianov <asterisk@dima.spb.ru>
 * and Steve Underwood <steveu@coppice.org>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 *
 */

/*** MODULEINFO
	 <depend>spandsp</depend>
***/
 
#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 209281 $")

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>
#include <tiffio.h>
#include <time.h>

#include <spandsp.h>
#ifdef HAVE_SPANDSP_EXPOSE_H
#include <spandsp/expose.h>
#endif

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/app.h"
#include "asterisk/dsp.h"
#include "asterisk/module.h"
#include "asterisk/manager.h"

static char *btxvst_app = "BTXvst";
static char *btxvst_synopsis = "Become a V23-Modem and act like a BTX Vermittlungsstelle.";
static char *btxvst_descrip = 
	"BTXvst(hostname,port,txpower,rxcutoff):  Pretends to be the old BTX Vermittlungsstelle\n"
	"in Ulm/Germany, operated by the Bundespost. The Hardware id of the\n"
	"terminal is requested and a connection to the specified host using the \n"
	"\"Ulm Relay Protocol\" is established.\n";


#define MAX_SAMPLES 240

typedef struct {
	struct ast_channel *chan;
	const char *host;	//ULMd host+port
	int port;
	float txpower;
	float rxcutoff;
	volatile int finished;
} btx_session;

#define BTX_BITBUFFER_SIZE 16
typedef struct {
	int answertone;		// terminal is active (=sends data)
	int nulsent;		// we sent a NULL as very first character (at least the DBT03 expects this)
} btxstuff;
typedef struct {
	int sock;
	int bitbuffer[BTX_BITBUFFER_SIZE];
	int writepos;
	int readpos;
	int fill;
	btxstuff *btx;
	btx_session *session;
} btx_data;


// this is called by spandsp whenever it filtered a new bit from the line
static void btx_put_bit(void *user_data, int bit) {
	int stop, i;

	btx_data *rx = (btx_data*) user_data;
	
	// terminal recognised us and starts responding through sending it's pilot tone
	// because line-noise may be interpreted as bits by spandsp we regard 10 consecutive
	// 1-bits as as the dbt-03s pilot tone, next step is to send a null-byte back
	if (rx->btx->answertone>0) {
		if (bit==1) {
			rx->btx->answertone--;
		}
		else if (bit==0) {
			rx->btx->answertone=10;
		}
	}
	else {
		// ignore other spandsp-stuff
		if (bit==1 || bit==0) {
			
			// insert bit into our bitbuffer
			rx->bitbuffer[rx->writepos]=bit;
			rx->writepos++;
			if (rx->writepos>=BTX_BITBUFFER_SIZE) rx->writepos=0;
			
			if (rx->fill<BTX_BITBUFFER_SIZE) {
				rx->fill++;
			} else { 
				// our bitbuffer is full, this probably won't happen
	// 			printf("full buffer\n");
				rx->readpos++;
				if (rx->readpos>=BTX_BITBUFFER_SIZE) rx->readpos=0;
			}
			
			// minimum 10 bits for full byte
			while (rx->fill>=10) {
				if (rx->bitbuffer[rx->readpos]==0) {	// check for startbit
					stop=(rx->readpos+9)%BTX_BITBUFFER_SIZE;
					if (rx->bitbuffer[stop]==1) {	// check for stopbit -> valid framing
						char byte=0;
						
						for(i=0; i<=7; i++) {	// generate byte
							if (rx->bitbuffer[(rx->readpos+1+i)%BTX_BITBUFFER_SIZE])
								byte |= (1<<i);
						}
						
						send(rx->sock, &byte, 1, 0);
						
						rx->readpos=(rx->readpos+10)%BTX_BITBUFFER_SIZE;
						rx->fill-=10;
						
					} else {	// no valid framing (no stopbit), remove first bit and maybe try again
						rx->fill--;
						rx->readpos++;
						if (rx->readpos>=BTX_BITBUFFER_SIZE) rx->readpos=0;
					}
				} else {	// no valid framing either (no startbit)
					rx->fill--;
					rx->readpos++;
					if (rx->readpos>=BTX_BITBUFFER_SIZE) rx->readpos=0;
				}
			}
		}
	}
	
	
	
	return;
}

// spandsp asks us for a bit to send onto the line
static int btx_get_bit(void *user_data) {
	btx_data *tx = (btx_data*) user_data;
	char byte=0;
	int i, rc;
	
	// no new data in send (bit)buffer, 
	// either we just picked up the line, the terminal started to respond, then we need to put a NULL-byte on the line first,
	// or we already sent that, than we check for new data on the socket
	// or there's no new data, so we send 1s (mark)
	if (tx->writepos==tx->readpos) {
		if(tx->btx->nulsent==1) {	// connection is established, look for data on socket
			rc=recv(tx->sock,&byte, 1, 0);
			if (rc>0) {
				// new data on socket, we put that byte into our bitbuffer
				for (i=0; i<=8; i++) {
					if (i>=8) tx->bitbuffer[tx->writepos]=1;	// stopbit
					else {	// databits
						if (byte & (1<<i)) tx->bitbuffer[tx->writepos]=1;
						else tx->bitbuffer[tx->writepos]=0;
					}
					tx->writepos++;
					if (tx->writepos>=BTX_BITBUFFER_SIZE) tx->writepos=0;
				}
				return 0; // return startbit immediately
			}
			else if (rc==0) {
				ast_log(LOG_WARNING,"Socket seems closed. Will hangup.\n");
				tx->session->finished=1;
			}
		}
		else {
			if ( tx->btx->answertone==0 ) {
// 				ast_log(LOG_WARNING,"Got TE's tone, will send null-byte.\n");
				// send null byte
				for (i=0; i<=8; i++) {
					if (i>=8) tx->bitbuffer[tx->writepos]=1;	//stopbit
					else {	//databits
						tx->bitbuffer[tx->writepos]=0;
					}
					tx->writepos++;
					if (tx->writepos>=BTX_BITBUFFER_SIZE) tx->writepos=0;
				}
				tx->btx->nulsent=1;
				
				// send ULM relay protocol header, we don't know anything about the caller, so it's rather empty
				send(tx->sock, "Version: 1\r\nTXspeed: 120\r\nRXspeed: 7.5\r\n\r\n", 42, 0);
				
				return 0;
			}
		}
		
		// no new data on socket, NULL-byte already sent, send mark-frequency 
		return 1;
		
	} else {
		// there still is data in the bitbuffer, so we just send that out
		i=tx->bitbuffer[tx->readpos];
		tx->readpos++;
		if (tx->readpos>=BTX_BITBUFFER_SIZE) tx->readpos=0;
// 		printf("b%i",i);
		return i;
	}
}

static void *btx_generator_alloc(struct ast_channel *chan, void *params)
{
	return params;
}

static int btx_generator_generate(struct ast_channel *chan, void *data, int len, int samples)
{
	fsk_tx_state_t *btx_tx = (fsk_tx_state_t*) data;
	uint8_t buffer[AST_FRIENDLY_OFFSET + MAX_SAMPLES * sizeof(uint16_t)];
	int16_t *buf = (int16_t *) (buffer + AST_FRIENDLY_OFFSET);
	
	struct ast_frame outf = {
		.frametype = AST_FRAME_VOICE,
		.subclass = AST_FORMAT_SLINEAR,
		.src = __FUNCTION__,
	};

	if (samples > MAX_SAMPLES) {
		ast_log(LOG_WARNING, "Only generating %d samples, where %d requested\n", MAX_SAMPLES, samples);
		samples = MAX_SAMPLES;
	}

	
	if ((len = fsk_tx(btx_tx, buf, samples)) > 0) {
		outf.samples = len;
		AST_FRAME_SET_BUFFER(&outf, buffer, AST_FRIENDLY_OFFSET, len * sizeof(int16_t));

		if (ast_write(chan, &outf) < 0) {
			ast_log(LOG_WARNING, "Failed to write frame to '%s': %s\n", chan->name, strerror(errno));
			return -1;
		}
	}

	return 0;
}

struct ast_generator generator = {
	alloc:		btx_generator_alloc,
	generate: 	btx_generator_generate,
};

static int btxvst_communicate(btx_session *s) {
	int res = -1;
	int original_read_fmt = AST_FORMAT_SLINEAR;
	int original_write_fmt = AST_FORMAT_SLINEAR;
	
	btx_data rxdata, txdata;
	
	struct ast_frame *inf = NULL;
	
	fsk_tx_state_t *btx_tx;
	fsk_rx_state_t *btx_rx;
	
	
	original_read_fmt = s->chan->readformat;
	if (original_read_fmt != AST_FORMAT_SLINEAR) {
		res = ast_set_read_format(s->chan, AST_FORMAT_SLINEAR);
		if (res < 0) {
			ast_log(LOG_WARNING, "Unable to set to linear read mode, giving up\n");
			return res;
		}
	}

	original_write_fmt = s->chan->writeformat;
	if (original_write_fmt != AST_FORMAT_SLINEAR) {
		res = ast_set_write_format(s->chan, AST_FORMAT_SLINEAR);
		if (res < 0) {
			ast_log(LOG_WARNING, "Unable to set to linear write mode, giving up\n");
			return res;
		}
	}
	
	int sock;
	struct sockaddr_in server;
	struct hostent *hp;
	struct ast_hostent ahp;
	
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if(sock < 0) {
		ast_log(LOG_WARNING, "Could not create socket.\n");
		return res;
	}
	
	server.sin_family=AF_INET;
	hp=ast_gethostbyname(s->host, &ahp);
	memcpy( (char *)&server.sin_addr, hp->h_addr, hp->h_length);
	//bcopy ( hp->h_addr, &(server.sin_addr.s_addr), hp->h_length);
	server.sin_port=htons(s->port);
	
	if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
		ast_log(LOG_WARNING, "Cannot connect to remote host.\n");
		return res;
	}
	
	fcntl(sock, F_SETFL, O_NONBLOCK);
	
	
	btxstuff btxcomm;
	btxcomm.answertone=10;
	btxcomm.nulsent=0;
	
	rxdata.sock=sock;
	rxdata.writepos=0;
	rxdata.readpos=0;
	rxdata.fill=0;
	rxdata.btx= &btxcomm;
	rxdata.session=s;
	
	txdata.sock=sock;
	txdata.writepos=0;
	txdata.readpos=0;
	txdata.fill=0;
	txdata.btx= &btxcomm;
	txdata.session=s;
	
	// initialise spandsp-stuff, give it our callback-functions
	btx_tx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_V23CH1], btx_get_bit, &txdata);
	fsk_tx_power (btx_tx, s->txpower);
	btx_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V23CH2], TRUE, btx_put_bit, &rxdata);
	fsk_rx_signal_cutoff(btx_rx, s->rxcutoff);
	
	//printf("comm: baud %i\n",btx_tx->baud_rate);
	ast_activate_generator(s->chan, &generator, btx_tx);
	
	while (!s->finished) {
		res = ast_waitfor(s->chan, 20);
		if (res < 0)
			break;
		else if (res > 0)
			res = 0;

		inf = ast_read(s->chan);
		if (inf == NULL) {
			ast_debug(1, "Channel hangup\n");
			res = -1;
			break;
		}
		
		/* Check the frame type. Format also must be checked because there is a chance
		   that a frame in old format was already queued before we set chanel format
		   to slinear so it will still be received by ast_read */
		if (inf->frametype == AST_FRAME_VOICE && inf->subclass == AST_FORMAT_SLINEAR) {
			if (fsk_rx(btx_rx, inf->data.ptr, inf->samples) < 0) {
				/* I know fax_rx never returns errors. The check here is for good style only */
				ast_log(LOG_WARNING, "btxvst returned error\n");
				res = -1;
				break;
			}
		}
		
		ast_frfree(inf);
		inf = NULL;
	}
	
	close(sock);
	
	if (original_write_fmt != AST_FORMAT_SLINEAR) {
		if (ast_set_write_format(s->chan, original_write_fmt) < 0)
			ast_log(LOG_WARNING, "Unable to restore write format on '%s'\n", s->chan->name);
	}

	if (original_read_fmt != AST_FORMAT_SLINEAR) {
		if (ast_set_read_format(s->chan, original_read_fmt) < 0)
			ast_log(LOG_WARNING, "Unable to restore read format on '%s'\n", s->chan->name);
	}

	return res;

}

static int btxvst_exec(struct ast_channel *chan, void *data) {
	int res = 0;
	char *parse;
	btx_session session;

	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(host);
		AST_APP_ARG(port);
		AST_APP_ARG(txpower);
		AST_APP_ARG(rxcutoff);
	);

	if (chan == NULL) {
		ast_log(LOG_ERROR, "Channel is NULL. Giving up.\n");
		return -1;
	}
	
	/* answer channel if not already answered */
	if (chan->_state != AST_STATE_UP) {
		res = ast_answer(chan);
		if (res) {
			ast_log(LOG_WARNING, "Could not answer channel '%s'\n", chan->name);
			return res;
		}
	}
	
	session.chan=chan;
	session.finished=0;
	
	parse=ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args,parse);
	
	if (args.host)
		session.host=args.host;
	else
		session.host="localhost";
	
	if (args.port)
		session.port=atoi(args.port);
	else
		session.port=8289;
	
	if (args.txpower)
		session.txpower=atof(args.txpower);
	else
		session.txpower=-30.0f;
	
	if (args.rxcutoff)
		session.rxcutoff=atof(args.rxcutoff);
	else
		session.rxcutoff=-70.0f;
	
	res=btxvst_communicate(&session);
	
	return res;
}



static int unload_module(void) {
	int res;
	res = ast_unregister_application(btxvst_app);
	return res;
}

static int load_module(void) {
	int res ;
	res = ast_register_application(btxvst_app, btxvst_exec, btxvst_synopsis, btxvst_descrip);
	return res;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "BTX Vermittlungsstelle",
		.load = load_module,
		.unload = unload_module,
		);