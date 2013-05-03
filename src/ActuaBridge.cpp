/*
 * ActuaBridge.cpp
 *
 *  Created on: 2013/05/03
 *      Author: Sabao Akutsu
 */

#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define QUE_KEY_NUM (key_t)1111
#define cmdSIZE 20
#define MAX_RETRY 5
#define MAX_WAIT 20

#define BAUDRATE 115200
#define MODEMDEVICE "/dev/ttyACM0"
#define _POSIX_SOURCE 1
#define FALSE 0
#define TRUE 1

static int fd = -1;
static int qid = -1;

static timespec req;
static timeval start, stop, re;
static timeval keep_alive_start, keep_alive_stop, keep_alive_re;
static timeval check_alive_start, check_alive_stop, check_alive_re;

volatile int STOP = FALSE;
volatile int SENDED = FALSE;
volatile int TEST_MODE = TRUE;
volatile int RECOVERY = FALSE;

char send_str[cmdSIZE] = { '\0' }; //送ったコマンド文字列の保持。

void sigcatch(int);
void disp_result(unsigned int, int);

typedef struct {
	long mtype;
	char data[cmdSIZE];
} T_Comm; //キューに使用する構造体。

void *sender(void* thdata) //コマンド送信スレッド
{
	T_Comm q_tmp = { 0, '\0' };

	int res, c_len;

	sleep(3);

	gettimeofday(&start, NULL);
	gettimeofday(&keep_alive_start, NULL);

	try {
		while (STOP == FALSE) {

			if (SENDED == FALSE) {

				if (msgrcv(qid, &q_tmp, sizeof(q_tmp.data), 1, IPC_NOWAIT)
						== -1) {

					nanosleep(&req, NULL);

				} else {
					c_len = strlen(q_tmp.data);
					q_tmp.data[c_len] = 0x0a; //改行（LF）をくっつける。

					res = write(fd, q_tmp.data, c_len + 1);
					strcpy(send_str, q_tmp.data);

					if (res != c_len + 1) {
						throw std::exception();
					} else if (res < 0) {
						throw std::exception();
					}

					memset(q_tmp.data, 0, sizeof(q_tmp.data));
					SENDED = TRUE;
					//一つのコマンド送信毎にCPUを開放してchecker()を実行させる。
					nanosleep(&req, NULL);
				}
			} else {
				nanosleep(&req, NULL);
			}
			//現時刻を取得。
			gettimeofday(&keep_alive_stop, NULL);
			//経過時間を計算。
			timersub(&keep_alive_stop, &keep_alive_start, &keep_alive_re);

			if (keep_alive_re.tv_usec > 30000) {
				//45ms毎にキープアライブ用の文字'~'（チルダ）を送信する。
				write(fd, "~\n", 2);
				//基準時刻を更新。
				gettimeofday(&keep_alive_start, NULL);
			}
		}

	} catch (std::exception &e) {
		STOP = TRUE;
	}
	std::cout << "\nSND:BYE." << std::endl;
	return (void *) NULL;
}

void *receiver(void* thdata) {
	int res;
	bool stay_flg = TRUE;
	bool alive_flg = TRUE;

	char buff[255] = { '\0' };

	T_Comm q_tmp = { 2, '\0' };

	gettimeofday(&check_alive_start, NULL);

	try {
		while (STOP == FALSE) {
			res = read(fd, buff, sizeof(buff));

			if (res < 0) {
				switch (errno) {
				case EAGAIN: {
					nanosleep(&req, NULL);
					break;
				}
				default: {
					throw std::exception();
				}
				}
			} else if (res <= cmdSIZE) {
				//受信データの処理。
				strcpy(q_tmp.data, buff);
				switch (q_tmp.data[0]) {
				case '>': //デバイスオブジェクトの受信確認エコー。
				case '^': //EMGCYモード強制解除。
					break;
				case '~': //キープアライブ文字。
				{
					stay_flg = FALSE;
					alive_flg = TRUE;
					break;
				}
				case '(': {

					//ここでセンサーが送出したデータのキュー振り分け処理を記述。

					break;
				}
				default: {
					//エコーをチェック用のキューに押し込む。
					if (msgsnd(qid, &q_tmp, sizeof(q_tmp.data), 0)) {
						perror("msgsnd");
						throw std::exception();
					}
					break;
				}
					memset(q_tmp.data, 0, sizeof(q_tmp.data));
				}
				memset(buff, 0, sizeof(buff));
			}

			if (stay_flg == FALSE) {
				//現時刻を取得。
				gettimeofday(&check_alive_stop, NULL);
				timersub(&check_alive_stop, &check_alive_start, &check_alive_re);
				//送信してから100ms経過してたらフラグをチェック。
				if (check_alive_re.tv_usec > 100000) {
					if (alive_flg == FALSE) {
						std::cout << "\nKeep-alive: Time out." << std::endl;
						RECOVERY = TRUE;
						throw std::exception();
					} else {
						alive_flg = FALSE;
						gettimeofday(&check_alive_start, NULL);
					}
				}

			}
		}

	} catch (std::exception &e) {
		STOP = TRUE;
	}

	std::cout << "\nRCV:BYE." << std::endl;

	return (void *) NULL;
}

void* checker(void* thdata) {

	unsigned int i = 0;
	unsigned int j = 0;
	unsigned int sended = 0;

	int wait_cnt = MAX_WAIT;
	int retry_cnt = MAX_RETRY;
	int retry_total = 0;
	int res = -1;
	int c_len = 0;

	T_Comm echo_str = { 0, '\0' };

	char ani_bar[4] = { '|', '/', '-', '\\', };

	std::cout << "Testing..." << std::flush;

	try {
		while (STOP == FALSE) {
			++i;
			if (SENDED == TRUE) {

				do {
					res = msgrcv(qid, &echo_str, sizeof(echo_str.data), 2,
							IPC_NOWAIT);

					--wait_cnt;

					if (res < 0) {
						if (wait_cnt < 0) {
							break;
						} else {
							nanosleep(&req, NULL);
						}
					}

				} while (res < 0);

				if (res < 0) {
					strcpy(echo_str.data, "No Echo.\n");
				}

				if (strcmp(send_str, echo_str.data) != 0) {

					--retry_cnt;
					++retry_total;
					tcflush(fd, TCIOFLUSH);

					c_len = strlen(send_str);
					res = write(fd, send_str, c_len);

					if (res != c_len) {
						throw std::exception();
					} else if (res < 0) {
						throw std::exception();
					}

					if (retry_cnt > 0) {
						continue;
					} //再送処理。

					std::cout << "failed.\n" << std::endl;
					std::cout << "Illegal echo." << std::endl;
					std::cout << "expected:\t" << send_str << std::flush;
					std::cout << "received:\t" << echo_str.data << std::endl;
					RECOVERY = TRUE;
					throw std::exception();

				} else {
					retry_cnt = MAX_RETRY;
					wait_cnt = MAX_WAIT;
					res = -1;
					sended++;

					memset(send_str, 0, sizeof(send_str));
					memset(echo_str.data, 0, sizeof(echo_str.data));

					if (sended == 500 && TEST_MODE == TRUE) {
						gettimeofday(&stop, NULL);
						timersub(&stop, &start, &re);
						std::cout << "succeed! ActuaBridge started.\n" << std::endl;
						disp_result(sended, retry_total);
						sended = 0;
						TEST_MODE = FALSE;

						//ここでテストモード終了。

						std::cout << "\nChecking echo CMD..." << std::flush;
					}
					SENDED = FALSE;
				}

			}

			if ((i % 200) == 0) {
				++j;
				std::cout << "\x1b[s" << std::flush;
				std::cout << ani_bar[j % 4] << std::flush;
				std::cout << "\x1b[u" << std::flush;
			}
			nanosleep(&req, NULL);
		}

	} catch (std::exception &e) {
		STOP = TRUE;
	}
	gettimeofday(&stop, NULL);
	timersub(&stop, &start, &re);
	disp_result(sended, retry_total);
	std::cout << "\nCHK:BYE." << std::endl;
	return (void *) NULL;
}
void* tester(void* thdata) {
	int i = 0;
	int rnd = 0;
	unsigned int seed = 1;

	timespec wait;
	wait.tv_sec = 0;
	wait.tv_nsec = 0;

	T_Comm testData[] = { { 1, "<1|2,p" }, { 1, "<1|3,p" }, { 1, "<1|1,200" },
			{ 1, "<1|2,d,0" }, { 1, "<1|3,d,0" }, { 1, "<1|1,600" }, { 1,
					"<1|2,d,180" }, { 1, "<1|3,d,180" }, };
	try {
		while (STOP == FALSE) {
			if (TEST_MODE == TRUE) {
				sleep(2);
				continue;
			}

			for (; i < 8; i++) {
				if (msgsnd(qid, &testData[i], sizeof(testData[i].data), 0)) {
					perror("msgsnd");
					throw std::exception();
				}
				rnd = (rand_r(&seed) % 8) + 1;
				wait.tv_nsec = rnd * 100000000;
				nanosleep(&wait, NULL);
			}
			i = 2;
		}
	} catch (std::exception &e) {
		STOP = TRUE;
	}

	std::cout << "\nTST:BYE." << std::endl;
	return (void *) NULL;
}

int main(int argc, char **argv) {
	int res;
	int recov_cnt = 5;
	unsigned int i;
	unsigned long int us_total = 0;
	pthread_t snd, rcv, chk, tst;

	T_Comm sndData[] = { { 1, "<1|1,200" }, { 1, "<2|1,200" },
			{ 1, "<1|1,1500" }, { 1, "<2|1,1500" }, };

	struct termios oldtio, newtio;

	timerclear(&start);
	timerclear(&stop);
	timerclear(&re);

	req.tv_sec = 0;
	req.tv_nsec = 1000000; // 1ms

	std::cout << "\x1b[2J" << std::flush;
	std::cout << "\x1b[0;0H" << std::flush;
	std::cout << "ActuaBridge 1.00 Copyright(C) 2013 by Sabao Akutsu\n"
			<< std::endl;
	try {
		if (SIG_ERR == signal(SIGINT, sigcatch)) {
			throw std::exception();
		}

		std::cout << "Open serial...\t\t" << std::flush;

		fd = open(MODEMDEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (fd < 0) {
			perror(MODEMDEVICE);
			throw std::exception();
		}

		std::cout << "OK." << std::endl;
		std::cout << "Setting serial...\t" << std::flush;

		tcgetattr(fd, &oldtio); /* 現在のシリアルポートの設定を待避させる */
		memset(&newtio, 0, sizeof(newtio)); /* 新しいポートの設定の構造体をクリアする */

		/* 新しいポートの設定をカノニカル入力処理に設定する */
		newtio.c_cflag = CS8 | CLOCAL | CREAD | PARENB;
		newtio.c_cc[VTIME] = 0;
		newtio.c_lflag = ICANON;
		newtio.c_iflag = IGNPAR | ICRNL;
		newtio.c_oflag = 0;
		cfsetispeed(&newtio, BAUDRATE);
		cfsetospeed(&newtio, BAUDRATE);

		tcflush(fd, TCIOFLUSH);
		tcsetattr(fd, TCSANOW, &newtio);

		std::cout << "OK." << std::endl;
		std::cout << "Wait a minuite" << std::flush;

		for (i = 0; i < 2000; i++) {
			gettimeofday(&start, NULL);

			nanosleep(&req, NULL);

			gettimeofday(&stop, NULL);
			timersub(&stop, &start, &re);

			us_total += re.tv_usec;

			if (i % 400 == 0) {
				std::cout << '.' << std::flush;
			}
		}
		std::cout << "\nPolling cycle is about " << us_total / 2000
				<< " microseconds.\n" << std::endl;

		timerclear(&start);
		timerclear(&stop);
		timerclear(&re);

		RECOVERING: std::cout << "Create queue...\t\t" << std::flush;

		/*キュー作成*/
		qid = msgget(QUE_KEY_NUM, IPC_CREAT | IPC_EXCL | 0666);
		if (qid == -1) {
			qid = msgget(QUE_KEY_NUM, IPC_EXCL | 0666);
			if (qid == -1) {
				perror("msgget");
				throw std::exception();
			}
		}

		std::cout << "OK." << std::endl;

		for (i = 0; i < 500; i++) {
			if (msgsnd(qid, &sndData[i % 4], sizeof(sndData[i % 4].data), 0)) {
				perror("msgsnd");
				throw std::exception();
			}
		}

		std::cout << "Create thread...\t" << std::flush;

		/* ここでコマンド送受信用スレッドの生成を行う。 */
		res = pthread_create(&snd, NULL, sender, NULL);
		if (res != 0) {
			throw std::exception();
		}

		res = pthread_create(&rcv, NULL, receiver, NULL);
		if (res != 0) {
			throw std::exception();
		}

		res = pthread_create(&chk, NULL, checker, NULL);
		if (res != 0) {
			throw std::exception();
		}

		res = pthread_create(&tst, NULL, tester, NULL);
		if (res != 0) {
			throw std::exception();
		}

		std::cout << "OK." << std::endl;

		/*main()はpthread_join()でスレッドの終了を待つ。 */
		pthread_join(rcv, NULL);
		pthread_join(snd, NULL);
		pthread_join(chk, NULL);
		pthread_join(tst, NULL);

		/*メッセージキュー破壊*/
		msgctl(qid, IPC_RMID, NULL);

		if (RECOVERY == TRUE && TEST_MODE == TRUE) {

			char res_str[255] = { '\0' };
			char* p = res_str;
			/*
			 一旦全てのサーバーを同じ状態（stayモード）にしてから対処する。
			 以下、サーバーが意図どおりの反応をしてくれれば、最大5回リカバリを行う。
			 */
			std::cout << "Starting to check server(s)..." << std::endl;
			sleep(3); //サーバー側がstayまたはEMGCYに落ち着くまで待つ。
			tcflush(fd, TCIOFLUSH);

			//リカバリ要求を送信。通信がうまく行っていれば'^'のエコーが全てのサーバーを貫通して戻ってくるはずである。
			//サーバーのどれかが既にstayの可能性があるので\nを付けて送信する。
			write(fd, "^\n", 2);

			if (recov_cnt > 0) {
				std::cout << "Waiting server(s) to react..." << std::endl;
				//受信バッファにゴミが残っている場合を考慮して5回読み込む。
				for (i = 0; i < 5; i++) {
					nanosleep(&req, NULL);

					res = read(fd, res_str, sizeof(res_str));

					if (res > 0) {
						std::cout << "Check string : " << res_str << std::flush;

						do {
							if (*p++ == '^') {
								//グローバルのデータを復元。
								//まだキープアライブは送信していないのでサーバー側はstay状態のはずである。
								std::cout << "Receive response!" << std::endl;
								STOP = FALSE;
								SENDED = FALSE;
								TEST_MODE = TRUE;
								RECOVERY = FALSE;
								qid = -1;
								memset(send_str, '\0', cmdSIZE);
								recov_cnt--;
								std::cout << "Trying to recovery..."
										<< std::endl;
								tcflush(fd, TCIOFLUSH);
								//禁断のgoto
								goto RECOVERING;
							}
						} while ((p - res_str) < res);
					}
					p = res_str;
				}
				std::cout << "No response..." << std::endl;
				throw std::exception();

			} else {
				throw std::exception();
			}
		}

		/* プログラム開始時のポート設定を復元する */
		std::cout << "Resetting serial." << std::endl;
		tcsetattr(fd, TCSANOW, &oldtio);
		std::cout << "Close serial." << std::endl;
		close(fd);

	} catch (std::exception &e) {
		if (qid != -1) {
			msgctl(qid, IPC_RMID, NULL);
		}
		if (fd >= 0) {
			tcsetattr(fd, TCSANOW, &oldtio);
			close(fd);
		}

		exit(EXIT_FAILURE);
	}

	std::cout << "ActuaBridge terminated." << std::endl;
	return 0;
}

void sigcatch(int sig) {
	STOP = TRUE;
}

void disp_result(unsigned int sended, int retry_total) {
	std::cout << "\t[-Result-]" << std::endl;
	std::cout << "\tseconds\t\t:" << re.tv_sec << std::endl;
	std::cout << "\tmilliseconds\t:" << re.tv_usec / 1000 << std::endl;
	std::cout << "\tsended-total\t:" << sended << std::endl;
	std::cout << "\tretry-total\t:" << retry_total << std::endl;
}
