#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

//g++ -I/usr/include/postgresql/ -I/usr/include/opencv4 -o main main.cpp `pkg-config --cflags gtk4` `pkg-config --libs gtk4` `pkg-config --cflags opencv4` `pkg-config --libs opencv4` -lpq -lpqxx -lportaudio -I/usr/include/portaudio
//g++ -I/usr/include/opencv4 -o main main.cpp `pkg-config --cflags opencv4` `pkg-config --libs opencv4`
#include <thread>
#include <iostream>
#include <ctime>
#include <time.h>
#include <string>
#include <chrono>

#include <fstream>


//PostgreSQL Lib
#include <libpq-fe.h>
#include <postgres_ext.h>

#include <pqxx/pqxx>

// OpenCV
#include <opencv2/opencv.hpp>

#include <portaudio.h>
#include <cstddef>

#include "b64/encode.h"

// Количество сэмплов за один буфер
#define SAMPLES_PER_BUFFER     (512)

/////////////////////////////////
GtkApplication *app;
GtkWidget *window_call_video;
GtkWidget *window_call_audio;
/////////////////////////////////

using namespace cv;
using namespace std;
using namespace pqxx;
using namespace  base64;

typedef struct {
	GtkWidget *username;
	GtkWidget *password;
	GtkWidget *message;
	GtkWidget *findText;
	const char* widget;
	char* idDialog;
} EntryData;


typedef struct {
	GtkWidget *gtkImage;
	GdkPixbuf *pixbuf;	
} EntryDataPix;

typedef struct {
	GtkWidget *username;
} EntryDataFind;

typedef struct {
	GtkWidget *username;
	GtkWidget *password;
	GtkWidget *rePassword;
} EntryDataSingUp;

PGconn *conn;

char*  glob_user_id = NULL;
char*  glob_user_username = NULL;
char*  glob_user_password = NULL;

char* glod_id_dialog;
char* timestamp_online_dialog;
char* timestamp_online_contacts;
char* timestamp_online;

int check_dialog = 0;
int check_list_contacts = 0;

GtkWidget *window;

GtkWidget *buttonBackToMessanger;
GtkWidget *buttonBackToAuthFromDialog;
GtkWidget *buttonSingIn;
GtkWidget *buttonSingUpFinish;

GtkWidget *boxLeftMenuListContacts;
GtkWidget *boxDialogChat;

int timer_id_contacts;
int timer_id_dialog;

static void auth (GtkWidget *widget);
static void main_messenger (GtkWidget *widget, EntryData *entryData);

char *rtrim(char *str) {
	if(str == NULL)  return NULL;
	char *end = str + strlen(str) - 1;
	while(end >= str && isspace(*end)) end--;
	*(end + 1) = '\0';
	return str;
}

void on_window_call_audio_destroy(GtkWidget *widget, gpointer data);

PaStream* streamAudio;

int ii_data = 0;

// Функция-колбэк обработки звука
static int patestCallback(const void* inputBuffer, void* outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void* userData)
{
    char* data = new char[framesPerBuffer * sizeof(float)];
    memcpy(data, inputBuffer, framesPerBuffer * sizeof(float));
    
	// Encode the data as Base64
	char* encodedData = new char[framesPerBuffer * sizeof(float) * 2]; // Allocate enough memory for the encoded data
	base64_encodestate state;
	base64_init_encodestate(&state);
	int encodedLength = base64_encode_block(data, framesPerBuffer * sizeof(float), encodedData, &state);
	encodedLength += base64_encode_blockend(encodedData + encodedLength, &state);

	// Convert the encoded data to bytea
	const char* byteaPrefix = "\\x"; // Prefix required for bytea values in PostgreSQL
	char* byteaData = new char[encodedLength * 2 + 3]; // Allocate enough memory for the bytea value
	byteaData[0] = '\'';
	byteaData[1] = '\\';
	byteaData[2] = 'x';
	for (int i = 0; i < encodedLength; i++) {
		sprintf(byteaData + i * 2 + 3, "%02x", (unsigned char)encodedData[i]);
	}
	byteaData[encodedLength * 2 + 3] = '\'';
	byteaData[encodedLength * 2 + 4] = '\0';

	// SQL запрос
	const char* query = "INSERT INTO audio_data (audio) VALUES ($1::bytea)";
	const char* paramValues[1] = { byteaData };
	int paramLengths[1] = { static_cast<int>(strlen(byteaData)) };
	int paramFormats[1] = { 1 };
	PGresult* res = PQexecParams(conn, query, 1, NULL, paramValues, paramLengths, paramFormats, 0);
    
    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
		printf("INSERT AUDIO \n");
	} else {
		printf("Failed INSERT AUDIO: %s\n", PQerrorMessage(conn));
	}
	PQclear(res);
	
 	usleep(2000000);
    
    // Изменяем тип буфера на нужный
    float* out = (float*)outputBuffer;
    const float* in = (const float*)inputBuffer;

    // Просто копируем входной буфер в выходной
    for (unsigned int i = 0; i < framesPerBuffer; i++)
    {
        out[i] = in[i];
    }
    
	delete[] data;
	delete[] encodedData;
	
    return paContinue;
}

void *capture_audio(void *data2)
{
    PaError err;
    
    float buffer[SAMPLES_PER_BUFFER];

    // Инициализируем PortAudio
    err = Pa_Initialize();
    if (err != paNoError) return NULL;

    // Открываем поток для записи и воспроизведения звука
    err = Pa_OpenDefaultStream(&streamAudio, 1, 1, paFloat32, 44100,
                               SAMPLES_PER_BUFFER, patestCallback, NULL);
    if (err != paNoError) goto error;

    // Запускаем поток
    err = Pa_StartStream(streamAudio);
    if (err != paNoError) goto error;

    // Цикл ожидания
    while (true)
    {
        // Задержка
        Pa_Sleep(1000);
    }

    // Останавливаем поток
    err = Pa_StopStream(streamAudio);
    if (err != paNoError) goto error;

    // Закрываем поток
    err = Pa_CloseStream(streamAudio);
    if (err != paNoError) goto error;

    // Освобождаем ресурсы PortAudio
    Pa_Terminate();

    return NULL;

error:
    Pa_Terminate();
    return NULL;
}

GThread *audio_thread;

void on_button_call_audio(GtkWidget *widget, EntryData *entryData) {
	
		on_window_call_audio_destroy(NULL, NULL);
		
		window_call_audio = gtk_application_window_new(app);
		gtk_window_set_title(GTK_WINDOW(window_call_audio), "Mamaq Messenger - Audio Call");
		gtk_window_set_default_size(GTK_WINDOW(window_call_audio), 200, 200);
		
		/*boxMainVideoCall = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		int width = 640;
		int height = 480;
		gtk_widget_set_size_request(GTK_WIDGET(boxMainVideoCall), width, height);
		gtk_window_set_child (GTK_WINDOW (window_call_video), boxMainVideoCall);
		
		gtkImage = GTK_IMAGE(gtk_image_new_from_pixbuf(pixbuf));
		gtk_widget_set_size_request(GTK_WIDGET(gtkImage), width, height);
		gtk_box_append(GTK_BOX(boxMainVideoCall), GTK_WIDGET(gtkImage));
		
		run_video = TRUE;*/
		
		gtk_widget_show(window_call_audio);
		
		// Запускаем поток для захвата видео
		audio_thread = g_thread_new("capture_thread", capture_audio, NULL);
		
		// установка обработчика закрытия окна
		g_signal_connect(window_call_audio, "destroy", G_CALLBACK(on_window_call_audio_destroy), NULL);
}

void on_window_call_audio_destroy(GtkWidget *widget, gpointer data) {   
    audio_thread = NULL;
    Pa_StopStream(streamAudio); 
    Pa_CloseStream(streamAudio);
}

/*
void remove_child(GtkWidget *widget, gpointer data) {
    GtkWidget *child = GTK_WIDGET(widget);
    gtk_container_remove(GTK_CONTAINER(data), child);
}*/

//void on_window_call_video_destroy(GtkWidget *widget, gpointer data);

// Объявляем переменные
gboolean run_video;
GThread *video_thread;
GtkImage *gtkImage;
GdkPixbuf *pixbuf;
VideoCapture cap;
GtkWidget *boxMainVideoCall;

void on_window_call_video_destroy(GtkWidget *widget, gpointer data);
void on_button_call_video(GtkWidget *widget, EntryData *entryData);


void gtk_pixbuf () {
		gtk_image_set_from_pixbuf (gtkImage, pixbuf);
}

// Функция для передачи видео в GTK image
void *capture_video(void *data) {

	Mat image;
	
	int ii = 0;
	
	if (cap.isOpened()){
    	
    	if (ii == 0) {
			GtkWidget *labelCameraNotWork = gtk_label_new("Camera not work or used other app.");
			gtk_window_set_child(GTK_WINDOW(window_call_video), GTK_WIDGET(labelCameraNotWork));
			gtk_widget_show(window_call_video);
					
			ii++;
		}
    	
    	while (cap.isOpened()) {
        	// Ждем пока камера не будет открыта
        	cap.release();
    	}
		
	} else {
	
		cap.open(0);
		
		
		if (!cap.isOpened()) {
			GtkWidget *labelCameraNotWork = gtk_label_new("Camera not work or used other app.");
			gtk_box_append(GTK_BOX(boxMainVideoCall), GTK_WIDGET(labelCameraNotWork));
			gtk_widget_show(window_call_video);
		}
		
		while (!cap.isOpened()) {
        	// Ждем пока камера не будет открыта
        	cap.open(0);
    	}
    	
		connection C("dbname=cpp_db user=postgres password=DB_PASSWORD hostaddr=127.0.0.1 port=5432");
		
		while(run_video) {
			
			if (cap.isOpened()){
				cap >> image;
				
				if (!image.empty()) {
					
					binarystring bs(image.data, image.cols * image.rows * image.channels());
					work W(C);
					W.exec("INSERT INTO video_stream (image_data) VALUES (" + W.quote(bs) + ")");
					W.commit();
					
					usleep(2000000);
					
					// конвертировать cv::Mat в массив байтов
					/*std::vector<uchar> buf;
					cv::imencode(".jpg", image, buf);
					const char* data = reinterpret_cast<const char*>(buf.data());
					size_t size = buf.size();

					unsigned char* byte_data = new unsigned char[size];
					std::copy(data, data + size, byte_data);

					unsigned char* escaped_data = PQescapeByteaConn(conn, byte_data, size, &size);
					
					delete[] byte_data;

					// вставить данные в БД
					const char* insert_query = "UPDATE video_stream SET image_data=$1 WHERE id=0";
					const char* params[1] = { (const char*)escaped_data };
					int param_lengths[1] = { static_cast<int>(size) };
					int param_formats[1] = { 1 };
					PGresult* res = PQexecParams(conn, insert_query, 1, NULL, params, param_lengths, param_formats, 0);
*/
					// освободить память
					//PQfreemem(escaped_data);

				/*	// обработать ошибки
					if (PQresultStatus(res) == PGRES_COMMAND_OK) {
						fprintf(stderr, "UPDATE: \n");
					} else {
						fprintf(stderr, "Failed to insert image. Error message: %s\n", PQerrorMessage(conn));
					}

					PQclear(res);
*/
				    pixbuf = gdk_pixbuf_new_from_data((guint8*)image.data, GDK_COLORSPACE_RGB, false, 8, image.cols, image.rows, image.step, nullptr, nullptr);
					g_idle_add((GSourceFunc)gtk_pixbuf, NULL);
				}
		    } else {
		    
		    	while (!cap.isOpened()) {
					// Ждем пока камера не будет открыта
					cap.open(0);
    			}
		    	/*if (ii == 0) {
					GtkWidget *labelCameraNotWork = gtk_label_new("Camera not work or used other app.");
					gtk_window_set_child(GTK_WINDOW(window_call_video), GTK_WIDGET(labelCameraNotWork));
					gtk_widget_show(window_call_video);
					
					ii++;
				}*/
				
		    	pixbuf = NULL;
		    }
		    usleep(1000);
		}
    }
    return NULL;
}

void on_window_call_video_destroy(GtkWidget *widget, gpointer data) {
   
    cap.release();
    
    while (cap.isOpened()) {
		// Ждем пока камера не будет открыта
		cap.release();
    }
    
    video_thread = NULL;
    
    run_video = FALSE;
    
    //g_thread_join(video_thread);
    
    gtkImage = NULL;
    pixbuf = NULL;
    
    GtkImage *gtkImage;
    GThread *video_thread;
	GdkPixbuf *pixbuf;
	
	while (gtk_widget_get_visible(GTK_WIDGET(window_call_video))) {
    	gtk_window_destroy(GTK_WINDOW(window_call_video));
	}
}

int timestamp_video_call[1000];
int video_call_num = 1;
int video_call_pre_num = 0;

// Функция обработчик кнопки
void on_button_call_video(GtkWidget *widget, EntryData *entryData) {
	
	if (video_call_num == 0) {
		timestamp_video_call[0] = time(NULL);
		timestamp_video_call[1] = time(NULL);
	} else {
		video_call_pre_num = video_call_num - 1;
		timestamp_video_call[video_call_pre_num] = timestamp_video_call[video_call_num] + 2;
		video_call_num++;
		
		timestamp_video_call[video_call_num] = time(NULL);
	}
	
	if (timestamp_video_call[video_call_num] >= timestamp_video_call[video_call_pre_num]) {
	
		on_window_call_video_destroy(NULL, NULL);
		
		window_call_video = gtk_application_window_new(app);
		gtk_window_set_title(GTK_WINDOW(window_call_video), "Mamaq Messenger - Video Call");
		gtk_window_set_default_size(GTK_WINDOW(window_call_video), 200, 200);
		
		boxMainVideoCall = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		int width = 640;
		int height = 480;
		gtk_widget_set_size_request(GTK_WIDGET(boxMainVideoCall), width, height);
		gtk_window_set_child (GTK_WINDOW (window_call_video), boxMainVideoCall);
		
		gtkImage = GTK_IMAGE(gtk_image_new_from_pixbuf(pixbuf));
		gtk_widget_set_size_request(GTK_WIDGET(gtkImage), width, height);
		gtk_box_append(GTK_BOX(boxMainVideoCall), GTK_WIDGET(gtkImage));
		
		run_video = TRUE;
		
		gtk_widget_show(window_call_video);
		
		// Запускаем поток для захвата видео
		video_thread = g_thread_new("capture_thread", capture_video, NULL);
		
		// установка обработчика закрытия окна
		g_signal_connect(window_call_video, "destroy", G_CALLBACK(on_window_call_video_destroy), NULL);
	}
}

static gboolean update_dialog(EntryData* entryData) {
	/*if (PQstatus(conn) != CONNECTION_OK) {
		printf("Connection failed: %s\n", PQerrorMessage(conn));
	}*/
	if (check_dialog == 0) {
		// Zero time for dialog load
		const std::string timestamp_str = "0";
		timestamp_online_dialog = new char[timestamp_str.length()+1];
		strcpy(timestamp_online_dialog, timestamp_str.c_str());
		
		check_dialog++;
	}
		
	//
	const char* auth_user_id = glob_user_id;
	const char* auth_user_username = glob_user_username;
	const char* user_dialog_id = glod_id_dialog;

	const Oid paramTypes[5] = {23, 23, 23, 23, 1043};
	const char* paramValues[5] = {auth_user_id, user_dialog_id, user_dialog_id, auth_user_id, timestamp_online_dialog};
	const int paramLengths[5] = {(int)strlen(auth_user_id), (int)strlen(user_dialog_id), (int)strlen(user_dialog_id), (int)strlen(auth_user_id), (int)strlen(timestamp_online_dialog)};
	const int paramFormats[5] = {0, 0, 0, 0, 0};

	const char* query = "SELECT * FROM message WHERE (id_reciever=$1 AND id_sender=$2) OR (id_reciever=$3 AND id_sender=$4) AND timestamp_post > $5 ORDER BY id LIMIT 5";

	PGresult *messageList = PQexecParams(conn, query, 5, paramTypes, paramValues, paramLengths, paramFormats, 0);
	
	if (PQresultStatus(messageList) == PGRES_TUPLES_OK && PQntuples(messageList) > 0) {
		
		GtkWidget *labelTextDialog[30];
		
		/*gint numChildren = 30;
		for (gint i = 0; i < numChildren; i++) {
			gtk_box_remove(GTK_BOX(boxDialogChat), GTK_WIDGET(labelTextDialog[i]));
		}*/
		
		//gtk_box_remove(GTK_BOX(boxDialogChat), GTK_WIDGET(labelTextDialog));
	
		
		printf("Good messageList");

		// 
		char trimmed_username[26]; // учитывая нулевой символ
		memset(trimmed_username, 0, sizeof(trimmed_username)); // обнуляем строку
		strncpy(trimmed_username, auth_user_username, sizeof(trimmed_username) - 1); // копируем строку и оставляем место для нулевого символа
		auth_user_username = rtrim(trimmed_username); // обрезаем пробелы справа

		// Создаем цикл для создания пяти меток с данными из messageList
		for (int i = 0; i < PQntuples(messageList); i++) {

			const char* db_message_id = PQgetvalue(messageList, i, PQfnumber(messageList, "id")); 				
			const char* db_message_id_sender = PQgetvalue(messageList, i, PQfnumber(messageList, "id_sender"));
			const char* db_message_id_reciever = PQgetvalue(messageList, i, PQfnumber(messageList, "id_reciever"));
			const char* db_message_message = PQgetvalue(messageList, i, PQfnumber(messageList, "message"));
					
			//printf("Message: %s \n ", db_message_message);
			
			const Oid paramTypes2[1] = {23};
			const char* paramValues2[1] = {db_message_id_sender};
			const int paramLengths2[1] = {(int)strlen(db_message_id_sender)};
			const int paramFormats2[1] = {0};
				
			const char* query2 = "SELECT username FROM users WHERE id=$1 LIMIT 1";
			
			PGresult *senderUsernameSql = PQexecParams(conn, query2, 1, paramTypes2, paramValues2, paramLengths2, paramFormats2, 0);
			
			const char* sender_username = PQgetvalue(senderUsernameSql, 0, PQfnumber(senderUsernameSql, "username")); 
			
			//
			char trimmed_db_message_message[101]; // учитывая нулевой символ
			memset(trimmed_db_message_message, 0, sizeof(trimmed_db_message_message)); // обнуляем строку
			strncpy(trimmed_db_message_message, db_message_message, sizeof(trimmed_db_message_message) - 1); // копируем строку и оставляем место для нулевого символа
			db_message_message = rtrim(trimmed_db_message_message); // обрезаем пробелы справа

			//
			char labelMessageView[100];
			memset(labelMessageView, 0, sizeof(labelMessageView));
			
			if (atoi(db_message_id_sender) == atoi(auth_user_id)) {
				strcat(labelMessageView, auth_user_username);
				strcat(labelMessageView, ": ");
				strcat(labelMessageView, db_message_message);
						
				labelTextDialog[i] = gtk_label_new(labelMessageView);
				gtk_box_append(GTK_BOX(boxDialogChat), labelTextDialog[i]);
				//printf("Message labelMessageView: %s \n ", db_message_message);
			} else {
				strcat(labelMessageView, sender_username);
				strcat(labelMessageView, ": ");
				strcat(labelMessageView, db_message_message);
						
				labelTextDialog[i] = gtk_label_new(labelMessageView);
				gtk_box_append(GTK_BOX(boxDialogChat), labelTextDialog[i]);
				//printf("Message labelMessageView: %s \n ", db_message_message);
			}
		}
		//gtk_box_remove(GTK_BOX(boxDialogChat), GTK_WIDGET(labelTextDialog));
	} else {
		printf("Not message. %s\n", PQerrorMessage(conn));
	}
	
	auto now = std::chrono::high_resolution_clock::now();
	auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
	const std::string timestamp_str = std::to_string(microseconds);
	delete[] timestamp_online_dialog;
	timestamp_online_dialog = new char[timestamp_str.length()+1];
	strcpy(timestamp_online_dialog, timestamp_str.c_str()); 
	
	gtk_widget_show(window);
	
	return G_SOURCE_CONTINUE;
}

void on_dialog_select(GtkWidget *widget, gpointer *id_user_dialog) {
	
	glod_id_dialog = (char*)g_object_get_data(G_OBJECT(widget), "user_id");
	
	//EntryData *entryData = (EntryData*)malloc(sizeof(EntryData));
	
	//main_messenger(widget, entryData);
}

static gboolean update_contacts_list(GtkWidget *widget) {

	if (check_list_contacts == 0) {
		// Zero time for dialog load
		const std::string timestamp_str = "0";
		timestamp_online_contacts = new char[timestamp_str.length()+1];
		strcpy(timestamp_online_contacts, timestamp_str.c_str());
		
		check_list_contacts++;
	}
	
	GtkWidget *labelContactsUser[30]; // создаем массив переменных типа GtkWidget
	GObject *objLabelContactsUser[30];
	int user_contacts_id[30];
	
	//const char* timestamp_online_check = timestamp_online;
	
	const Oid paramTypes[3] = {23, 23, 1043};
	const char* paramValues[3] = {glob_user_id, glob_user_id, timestamp_online_contacts};
	const int paramLengths[3] = {(int)strlen(glob_user_id), (int)strlen(glob_user_id), (int)strlen(timestamp_online_contacts)};
	const int paramFormats[3] = {0, 0, 0};

	const char* query = "SELECT * FROM list_contacts WHERE (id_user_requested=$1 OR id_user_accepted=$2) AND timestamp_send > $3 LIMIT 5";

	PGresult *contactsList = PQexecParams(conn, query, 3, paramTypes, paramValues, paramLengths, paramFormats, 0);

	// Если ЛОГИН и ПАРОЛЬ совпадает
	if (PQresultStatus(contactsList) == PGRES_TUPLES_OK && PQntuples(contactsList) > 0) {

		for (int i = 0; i < PQntuples(contactsList); i++) {
		
			char* id_user_requested = PQgetvalue(contactsList, i, PQfnumber(contactsList, "id_user_requested"));
			char* id_user_accepted = PQgetvalue(contactsList, i, PQfnumber(contactsList, "id_user_accepted"));
			
			char* user_contacts_id;
			
			//memset(user_contacts_id, 0, sizeof(user_contacts_id));
			
			if (atoi(id_user_requested) != atoi(glob_user_id)) {
				user_contacts_id = id_user_requested;
			} else {
				user_contacts_id = id_user_accepted;
			}
			
			const Oid paramTypes2[1] = {23};
			const char* paramValues2[1] = {user_contacts_id};
			const int paramLengths2[1] = {(int)strlen(user_contacts_id)};
			const int paramFormats2[1] = {0};
			
			const char* query2 = "SELECT username, id FROM users WHERE id=$1 LIMIT 1";
			
			PGresult *userContactsUsername = PQexecParams(conn, query2, 1, paramTypes2, paramValues2, paramLengths2, paramFormats2, 0);
			
			
			char* user_contacts_username = PQgetvalue(userContactsUsername, 0, PQfnumber(userContactsUsername, "username"));
			//char* user_contacts_id = PQgetvalue(userContactsUsername, 0, PQfnumber(userContactsUsername, "id"));
			
			if (PQresultStatus(userContactsUsername) == PGRES_TUPLES_OK && PQntuples(userContactsUsername) > 0) {
				printf("GOOD CONTACTS USERNAME %s\n", user_contacts_username);
			} else {
				printf("NO CONTACTS USERNAME\n");
			}
			
			//char buffer[32];
			//sprintf(buffer, "User %s", user_contacts_username); // генерируем название для каждой переменной
		
			char* textUserList = g_strdup_printf("<a href=\"\">%s</a>", user_contacts_username);
				
			labelContactsUser[i] = gtk_label_new(textUserList);
			//gtk_label_set_selectable(GTK_LABEL(labelContactsUser[i]), TRUE);
			gtk_label_set_use_markup(GTK_LABEL(labelContactsUser[i]), TRUE);  // Разрешаем использование разметки
			gtk_label_set_use_underline(GTK_LABEL(labelContactsUser[i]), true);
		
			// Установка пользовательских данных
			objLabelContactsUser[i] = G_OBJECT(labelContactsUser[i]);
			g_object_set_data(G_OBJECT(objLabelContactsUser[i]), "user_id", (gpointer*)user_contacts_id);
			
			g_signal_connect(labelContactsUser[i], "activate-link", G_CALLBACK(on_dialog_select), NULL);
			gtk_box_append(GTK_BOX(boxLeftMenuListContacts), labelContactsUser[i]);
		}
	} else {
		printf("NO CONTACTS\n");
	}
	
	auto now = std::chrono::high_resolution_clock::now();
	auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
	const std::string timestamp_str = std::to_string(microseconds);
	delete[] timestamp_online_contacts;
	timestamp_online_contacts = new char[timestamp_str.length()+1];
	strcpy(timestamp_online_contacts, timestamp_str.c_str());
	
	gtk_widget_show(window);
	
	return G_SOURCE_CONTINUE;
}

static void on_button_send_message (GtkWidget *widget, EntryData *entryData) {

	const char* auth_user_id = glob_user_id;
	const char* auth_user_username = glob_user_username;
	const char* auth_password = glob_user_password;
	const char* message = gtk_editable_get_text(GTK_EDITABLE(entryData->message));
	
	const Oid paramTypes[2] = {1043, 1043};
	const char* paramValues[2] = {auth_user_username, auth_password};
	const int paramLengths[2] = {(int)strlen(auth_user_username), (int)strlen(auth_password)};
	const int paramFormats[2] = {0, 0};

	const char* query = "SELECT * FROM users WHERE username=$1 AND password=$2 LIMIT 1";

	PGresult *user = PQexecParams(conn, query, 2, paramTypes, paramValues, paramLengths, paramFormats, 0);

	// Если ЛОГИН и ПАРОЛЬ совпадает
	if (PQresultStatus(user) == PGRES_TUPLES_OK && PQntuples(user) > 0) {
		//g_print("Auth successfully.");
		
		//entryData->idDialog = (char*)glob_user_id;
		
		const char* id_reciever = glod_id_dialog;
		
		auto now = std::chrono::high_resolution_clock::now();
		auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
		const std::string timestamp_str = std::to_string(microseconds);
		const char* timestamp_post = timestamp_str.c_str();
		
		//g_print("Timestamp: %lld\n", timestamp_post);

		const Oid paramTypesSM[4] = {23, 23, 1043, 1043}; // изменение типа первого параметра на integer (23)
		const char* paramValuesSM[4] = {auth_user_id, id_reciever, message, timestamp_post};
		const int paramLengthsSM[4] = {(int)strlen(auth_user_id), (int)strlen(id_reciever), (int)strlen(message), (int)strlen(timestamp_post)};
		const int paramFormatsSM[4] = {0, 0, 0, 0};

		const char* querySM = "INSERT INTO message (id_sender, id_reciever, message, timestamp_post) VALUES ($1, $2, $3, $4)";
		PGresult *sendMessageStatus = PQexecParams(conn, querySM, 4, paramTypesSM, paramValuesSM, paramLengthsSM, paramFormatsSM, 0);

		// обработка результата запроса
		if (PQresultStatus(sendMessageStatus) != PGRES_COMMAND_OK) {
			g_print("Error sending message: %s\n", PQerrorMessage(conn));
		}

		// очистка результата запроса
		PQclear(sendMessageStatus);
	}/* else {
		printf("Error inserting data: %s\n", PQerrorMessage(conn));
	}*/
}

static void on_add_contact (GtkWidget *widget, gpointer add_contact_id) {

	const char* add_contact_user_id = (const char*)add_contact_id;
	const char* user_id = glob_user_id;
	const Oid paramTypes[4] = {23, 23, 23, 23};
	const char* paramValues[4] = {user_id, add_contact_user_id, add_contact_user_id, user_id};
	const int paramLengths[4] = {(int)strlen(user_id), (int)strlen(add_contact_user_id), (int)strlen(add_contact_user_id), (int)strlen(user_id)};
	const int paramFormats[4] = {0, 0, 0, 0};

	const char* query = "SELECT id FROM list_contacts WHERE (id_user_requested=$1 AND id_user_accepted=$2) OR (id_user_requested=$3 AND id_user_accepted=$4)";
	
	PGresult *searchCheckContact = PQexecParams(conn, query, 4, paramTypes, paramValues, paramLengths, paramFormats, 0);
	
	
	
	if (PQresultStatus(searchCheckContact) == PGRES_TUPLES_OK && PQntuples(searchCheckContact) <= 0) {
	
		//printf(" добавлении записи: %s %s\n", add_contact_user_id, user_id);
	
		auto now = std::chrono::high_resolution_clock::now();
		auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
		const std::string timestamp_str = std::to_string(microseconds);
		delete[] timestamp_online;
		timestamp_online = new char[timestamp_str.length()+1];
		strcpy(timestamp_online, timestamp_str.c_str()); 
		
		const Oid paramTypes2[3] = {23, 23, 1043};
		const char* paramValues2[3] = {user_id, add_contact_user_id, timestamp_online};
		const int paramLengths2[3] = {(int)strlen(user_id), (int)strlen(add_contact_user_id), (int)strlen(timestamp_online)};
		const int paramFormats2[3] = {0, 0, 0};

		const char* query2 = "INSERT INTO list_contacts (id_user_requested, id_user_accepted, status, timestamp_send) VALUES ($1, $2, 'wait', $3)";
		
		PGresult *addContactSql = PQexecParams(conn, query2, 3, paramTypes2, paramValues2, paramLengths2, paramFormats2, 0);

		/*if (PQresultStatus(addContactSql) == PGRES_COMMAND_OK) {
			printf("Запись успешно добавлена.\n");
		} else {
			printf("Возникла ошибка при добавлении записи: %s\n", PQerrorMessage(conn));
		}*/
		PQclear(addContactSql);
	}/* else {
		printf("This contact have in list. %s\n", PQerrorMessage(conn));
	}*/
	PQclear(searchCheckContact);
}

static void on_profile_user (GtkWidget *widget) {
	
	const char* find_username_contact = (const char*)g_object_get_data(G_OBJECT(widget), "username");
	const Oid paramTypes[1] = {1043};
	const char* paramValues[1] = {find_username_contact};
	const int paramLengths[1] = {(int)strlen(find_username_contact)};
	const int paramFormats[1] = {0};

	const char* query = "SELECT * FROM users WHERE username=$1 LIMIT 1";

	PGresult *search = PQexecParams(conn, query, 1, paramTypes, paramValues, paramLengths, paramFormats, 0);

	// Если ЛОГИН и ПАРОЛЬ совпадает
	if (PQresultStatus(search) == PGRES_TUPLES_OK && PQntuples(search) > 0) {
		const char* contacts_user_username = PQgetvalue(search, 0, PQfnumber(search, "username"));
		const char* contacts_user_id = PQgetvalue(search, 0, PQfnumber(search, "id"));
		
		//printf("Connection!: %s\n %s\n", contacts_user_username, contacts_user_id);

		gpointer add_contact_id = (gpointer)contacts_user_id;

		GtkWidget *boxMain = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
		gtk_window_set_child (GTK_WINDOW (window), boxMain);
		

		// Создание метки find
		GtkWidget *labelNameSearchResult = gtk_label_new(contacts_user_username);
		gtk_box_append(GTK_BOX(boxMain), labelNameSearchResult);

		// Создание кнопки отправки
		GtkWidget *buttonAddContact = gtk_button_new_with_label("Add Contact");
		g_signal_connect(buttonAddContact, "clicked", G_CALLBACK(on_add_contact), add_contact_id);
		gtk_box_append(GTK_BOX(boxMain), buttonAddContact);
		
		EntryData *entryData = (EntryData*)malloc(sizeof(EntryData));
		//entryData->widget = "find";
		
		// Создание кнопки отправки
		buttonBackToMessanger = gtk_button_new_with_label("Back To Messenger");
		g_signal_connect(buttonBackToMessanger, "clicked", G_CALLBACK(main_messenger), entryData);
		gtk_box_append(GTK_BOX(boxMain), buttonBackToMessanger);
		
		// Отображение окна и всех элементов
		gtk_widget_show(window);
	}
}

static void on_serach (GtkWidget *widget, EntryData *entryData) {

	const char* find_text = gtk_editable_get_text(GTK_EDITABLE(entryData->findText));
	
	char* find_user = (char*)malloc(sizeof(find_user));
	strcpy(find_user, find_text);
	
	//printf("find: %s\n", find_user);
	const Oid paramTypes[1] = {1043};
	const char* paramValues[1] = {find_text};
	const int paramLengths[1] = {(int)strlen(find_text)};
	const int paramFormats[1] = {0};

	const char* query = "SELECT * FROM users WHERE username=$1 LIMIT 1";

	PGresult *search = PQexecParams(conn, query, 1, paramTypes, paramValues, paramLengths, paramFormats, 0);

	// Если ЛОГИН и ПАРОЛЬ совпадает
	if (PQresultStatus(search) == PGRES_TUPLES_OK && PQntuples(search) > 0) {

		const char* search_result_username = PQgetvalue(search, 0, PQfnumber(search, "username"));
		
		g_source_remove(timer_id_dialog);
		
		g_source_remove(timer_id_contacts);

		//gtk_container_foreach(GTK_CONTAINER(window), remove_boxes, window);

		GtkWidget *boxMain = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
		gtk_window_set_child (GTK_WINDOW (window), boxMain);

		// Создание метки find
		GtkWidget *labelNameSearchResult = gtk_label_new("Search Result");
		gtk_box_append(GTK_BOX(boxMain), labelNameSearchResult);
		
		const char* textSearchResult = g_strdup_printf("<a href=\"\">%s</a>", search_result_username);
		
		GtkWidget *labelSearchResult = gtk_label_new(textSearchResult);
		gtk_label_set_use_markup(GTK_LABEL(labelSearchResult), TRUE);  // Разрешаем использование разметки
		gtk_label_set_use_underline(GTK_LABEL(labelSearchResult), true);
		// Установка пользовательских данных
		
		GObject *objLabelSearchResult = G_OBJECT(labelSearchResult);
		g_object_set_data(G_OBJECT(objLabelSearchResult), "username", find_user);
		
		g_signal_connect(labelSearchResult, "activate-link", G_CALLBACK(on_profile_user), NULL);
		gtk_box_append(GTK_BOX(boxMain), labelSearchResult);

		GtkWidget *entrySearch = gtk_entry_new();
		gtk_entry_set_placeholder_text(GTK_ENTRY(entrySearch), "What find?");
		gtk_box_append(GTK_BOX(boxMain), entrySearch);

		entryData->findText = entrySearch;

		// Создание кнопки отправки
		GtkWidget *buttonSearch = gtk_button_new_with_label("Find!");
		g_signal_connect(buttonSearch, "clicked", G_CALLBACK(on_serach), entryData);
		gtk_box_append(GTK_BOX(boxMain), buttonSearch);

		// Создание кнопки отправки
		buttonBackToMessanger = gtk_button_new_with_label("Back");
		g_signal_connect(buttonBackToMessanger, "clicked", G_CALLBACK(main_messenger), entryData);
		gtk_box_append(GTK_BOX(boxMain), buttonBackToMessanger);

		// Отображение окна и всех элементов
		gtk_widget_show(window);

		PQclear(search);
	}
	/*else
	{
		printf("Failed: %s\n", PQerrorMessage(conn));
	}*/
}


void main_messenger(GtkWidget *widget, EntryData *entryData) {
	PGresult *user;

	if (glob_user_username != NULL && glob_user_password != NULL){
		const char* username_g = glob_user_username;
		const char* password_g = glob_user_password;
		const Oid paramTypes[2] = {1043, 1043};
		const char* paramValues[2] = {username_g, password_g};
		const int paramLengths[2] = {(int)strlen(username_g), (int)strlen(password_g)};
		const int paramFormats[2] = {0, 0};

		const char* query = "SELECT * FROM users WHERE username=$1 AND password=$2 LIMIT 1";

		user = PQexecParams(conn, query, 2, paramTypes, paramValues, paramLengths, paramFormats, 0);
	  } else {
		const Oid paramTypes[2] = {1043, 1043};
		const char *username = gtk_editable_get_text(GTK_EDITABLE(entryData->username));
		const char *password = gtk_editable_get_text(GTK_EDITABLE(entryData->password));
		const char* paramValues[2] = {username, password};
		const int paramLengths[2] = {(int)strlen(username), (int)strlen(password)};
		const int paramFormats[2] = {0, 0};

		const char* query = "SELECT * FROM users WHERE username=$1 AND password=$2 LIMIT 1";

		user = PQexecParams(conn, query, 2, paramTypes, paramValues, paramLengths, paramFormats, 0);
	  }

	  // Если ЛОГИН и ПАРОЛЬ совпадает
	  if (PQresultStatus(user) == PGRES_TUPLES_OK && PQntuples(user) > 0) {
		// обработка введенных данных

		//
		const char* db_user_id = PQgetvalue(user, 0, PQfnumber(user, "id"));
		const char* db_user_username = PQgetvalue(user, 0, PQfnumber(user, "username"));
		const char* db_user_password = PQgetvalue(user, 0, PQfnumber(user, "password"));

		glob_user_id = new char[strlen(db_user_id) + 1];
		strcpy(glob_user_id, db_user_id);
		glob_user_username = new char[strlen(db_user_username) + 1];
		strcpy(glob_user_username, db_user_username);
		glob_user_password = new char[strlen(db_user_password) + 1];
		strcpy(glob_user_password, db_user_password);

		
		GtkWidget *boxMain = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
		gtk_window_set_child (GTK_WINDOW (window), boxMain);

		GtkWidget *boxLeftMenu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
		gtk_box_append(GTK_BOX(boxMain), boxLeftMenu);
		
		GtkWidget *boxMiddle = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
		gtk_box_append(GTK_BOX(boxMain), boxMiddle);

		GtkWidget *boxLeftMenuFind = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
		gtk_box_append(GTK_BOX(boxLeftMenu), boxLeftMenuFind);
		
		boxLeftMenuListContacts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
		gtk_box_append(GTK_BOX(boxLeftMenu), boxLeftMenuListContacts);
		
		const Oid paramTypesSDU[1] = {23};
		const char* paramValuesSDU[1] = {glod_id_dialog};
		const int paramLengthsSDU[1] = {(int)strlen(glod_id_dialog)};
		const int paramFormatsSDU[1] = {0};

		const char* querySDU = "SELECT username FROM users WHERE id=$1 LIMIT 1";

		PGresult *search_dialog_username_sql = PQexecParams(conn, querySDU, 1, paramTypesSDU, paramValuesSDU, paramLengthsSDU, paramFormatsSDU, 0);
		
		const char* search_dialog_username = PQgetvalue(search_dialog_username_sql, 0, PQfnumber(search_dialog_username_sql, "username"));
		
		/*if (PQresultStatus(search_dialog_username_sql) == PGRES_TUPLES_OK && PQntuples(search_dialog_username_sql) > 0) {
		g_print("Found %s\n", search_dialog_username);
		} else {
			g_print("Not found: %s\n", PQerrorMessage(conn));
		}*/
		
		// Создание метки
		GtkWidget *labelDialogUsername = gtk_label_new(search_dialog_username);
		gtk_label_set_selectable(GTK_LABEL(labelDialogUsername), TRUE);
		gtk_box_append(GTK_BOX(boxMiddle), labelDialogUsername);
		
		boxDialogChat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
		gtk_box_append(GTK_BOX(boxMiddle), boxDialogChat);
		
		
		GtkWidget *boxDialogMenu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
		gtk_box_append(GTK_BOX(boxMiddle), boxDialogMenu);	
		
		
		// Создание метки find
		GtkWidget *labelSearch = gtk_label_new("Search:");
		gtk_box_append(GTK_BOX(boxLeftMenuFind), labelSearch);
		
		GtkWidget *entrySearch = gtk_entry_new();
		gtk_entry_set_placeholder_text(GTK_ENTRY(entrySearch), "What find?");
		gtk_box_append(GTK_BOX(boxLeftMenuFind), entrySearch);
		
		entryData->findText = entrySearch;
		
		// Создание кнопки отправки
		GtkWidget *buttonSearch = gtk_button_new_with_label("Find!");
		g_signal_connect(buttonSearch, "clicked", G_CALLBACK(on_serach), entryData);
		gtk_box_append(GTK_BOX(boxLeftMenuFind), buttonSearch);
		
		
		check_dialog = 0;
		check_list_contacts = 0;

		// Create a timer to update the buttons
		timer_id_contacts = g_timeout_add_seconds(1, (GSourceFunc)update_contacts_list, NULL);

		timer_id_dialog = g_timeout_add_seconds(1, (GSourceFunc)update_dialog, entryData);
		
		
		// Создание кнопки отправки
		GtkWidget *buttonCallVideo = gtk_button_new_with_label("Call Video");
		g_signal_connect(buttonCallVideo, "clicked", G_CALLBACK(on_button_call_video), entryData);
		gtk_box_append(GTK_BOX(boxDialogMenu), buttonCallVideo);
		
		// Создание кнопки отправки
		GtkWidget *buttonCallAudio = gtk_button_new_with_label("Call Audio");
		g_signal_connect(buttonCallAudio, "clicked", G_CALLBACK(on_button_call_audio), entryData);
		gtk_box_append(GTK_BOX(boxDialogMenu), buttonCallAudio);

		// Создание метки
		GtkWidget *labelUsername = gtk_label_new(glob_user_username);
		gtk_label_set_selectable(GTK_LABEL(labelUsername), TRUE);
		gtk_box_append(GTK_BOX(boxDialogMenu), labelUsername);

		GtkWidget *message = gtk_entry_new();
		gtk_entry_set_placeholder_text(GTK_ENTRY(message), "Enter your message here");
		gtk_box_append(GTK_BOX(boxDialogMenu), message);

		entryData->message = message;
		entryData->idDialog = glod_id_dialog;

		// Создание кнопки отправки
		GtkWidget *buttonSendMessage = gtk_button_new_with_label("Send");
		g_signal_connect(buttonSendMessage, "clicked", G_CALLBACK(on_button_send_message), entryData);
		gtk_box_append(GTK_BOX(boxDialogMenu), buttonSendMessage);

		buttonBackToAuthFromDialog = gtk_button_new_with_label("Log Out");
		g_signal_connect(buttonBackToAuthFromDialog, "clicked", G_CALLBACK(auth), NULL);
		gtk_box_append(GTK_BOX(boxDialogMenu), buttonBackToAuthFromDialog);

		// Отображение окна и всех элементов
		gtk_widget_show(window);
	}
}

void on_button_sing_up_finish(GtkWidget *widget, EntryDataSingUp *entryDataSingUp) {

	const char *username = gtk_editable_get_text(GTK_EDITABLE(entryDataSingUp->username));
	const char *password = gtk_editable_get_text(GTK_EDITABLE(entryDataSingUp->password));
	
	const Oid paramTypesC[1] = {1043}; // изменение типа первого параметра на integer (23)
	const char* paramValuesC[1] = {username};
	const int paramLengthsC[1] = {(int)strlen(username)};
	const int paramFormatsC[1] = {0};

	const char* queryC = "SELECT id FROM users WHERE username=$1";
	PGresult *singUpCheckUsername = PQexecParams(conn, queryC, 1, paramTypesC, paramValuesC, paramLengthsC, paramFormatsC, 0);
	
	if (PQresultStatus(singUpCheckUsername) == PGRES_TUPLES_OK && PQntuples(singUpCheckUsername) <= 0) {
		//const char db_user_username_check = PQgetvalue(singUpCheckUsername, 0, PQfnumber(singUpCheckUsername, "username"));
		
		const Oid paramTypesSM[2] = {1043, 1043}; // изменение типа первого параметра на integer (23)
		const char* paramValuesSM[2] = {username, password};
		const int paramLengthsSM[2] = {(int)strlen(username), (int)strlen(password)};
		const int paramFormatsSM[2] = {0, 0};

		const char* querySM = "INSERT INTO users (username, password) VALUES ($1, $2)";
		PGresult *singUpFinishStatus = PQexecParams(conn, querySM, 2, paramTypesSM, paramValuesSM, paramLengthsSM, paramFormatsSM, 0);

		// обработка результата запроса
		if (PQresultStatus(singUpFinishStatus) == PGRES_COMMAND_OK) {
			g_print("Registred!\n");
			
			EntryData *entryData = (EntryData*)malloc(sizeof(EntryData));
			entryData->username = entryDataSingUp->username;
			entryData->password = entryDataSingUp->password;
			
			glob_user_username = NULL;
			glob_user_password = NULL;
			
			main_messenger(widget, entryData);
		}
	}
}

void on_button_sing_up(GtkWidget *widget, EntryData *entryData) {
	GtkWidget *boxSingUp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
	gtk_window_set_child (GTK_WINDOW (window), boxSingUp);

	// Создание метки
	GtkWidget *labelSingUp = gtk_label_new("Sing Up");
	// Добавление метки в контейнер перед полем ввода сообщения
	gtk_box_append(GTK_BOX(boxSingUp), labelSingUp);
		
	// Создание первого поля ввода
	GtkWidget *usernameSingUp = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(usernameSingUp), "Enter username here");
	gtk_box_append(GTK_BOX(boxSingUp), usernameSingUp);

	// Создание второго поля ввода
	GtkWidget *passwordSingUp = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(passwordSingUp), "Enter password here");
	gtk_box_append(GTK_BOX(boxSingUp), passwordSingUp);
		
	// Создание второго поля ввода
	GtkWidget *rePasswordSingUp = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(rePasswordSingUp), "Enter password here");
	gtk_box_append(GTK_BOX(boxSingUp), rePasswordSingUp);
	
	EntryDataSingUp *entryDataSingUp = (EntryDataSingUp*)malloc(sizeof(EntryDataSingUp));
	entryDataSingUp->username = usernameSingUp;
	entryDataSingUp->password = passwordSingUp;
	entryDataSingUp->password = rePasswordSingUp;
	
	// Создание кнопки
	buttonSingUpFinish = gtk_button_new_with_label("Sing Up");
	g_signal_connect(buttonSingUpFinish, "clicked", G_CALLBACK(on_button_sing_up_finish), entryDataSingUp);
	gtk_box_append(GTK_BOX(boxSingUp), buttonSingUpFinish);
	
	// Создание кнопки
	GtkWidget *buttonBackToAuthFromSingUp = gtk_button_new_with_label("Back");
	g_signal_connect(buttonBackToAuthFromSingUp, "clicked", G_CALLBACK(auth), NULL);
	gtk_box_append(GTK_BOX(boxSingUp), buttonBackToAuthFromSingUp);
	
	// Отображение окна и всех элементов
	gtk_widget_show(window);
}

static void
auth (GtkWidget *widget)
{
	if (widget == buttonBackToAuthFromDialog) {
		g_source_remove(timer_id_dialog);	
		g_source_remove(timer_id_contacts);
		glob_user_username = NULL;
		glob_user_password = NULL;
	}

    GtkWidget *boxAuth = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_window_set_child (GTK_WINDOW (window), boxAuth);
	
	// Создание метки
	GtkWidget *labelSingIn = gtk_label_new("Sing In");
	// Добавление метки в контейнер перед полем ввода сообщения
	gtk_box_append(GTK_BOX(boxAuth), labelSingIn);
	
	// Создание первого поля ввода
	GtkWidget *username = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(username), "Enter username here");
	gtk_box_append(GTK_BOX(boxAuth), username);

	// Создание второго поля ввода
	GtkWidget *password = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(password), "Enter password here");
	gtk_box_append(GTK_BOX(boxAuth), password);

	EntryData *entryData = (EntryData*)malloc(sizeof(EntryData));
	entryData->username = username;
	entryData->password = password;

	// Создание кнопки
	buttonSingIn = gtk_button_new_with_label("Sing In");
	g_signal_connect(buttonSingIn, "clicked", G_CALLBACK(main_messenger), entryData);
	gtk_box_append(GTK_BOX(boxAuth), buttonSingIn);
	
	// Создание кнопки
	GtkWidget *buttonSingUp = gtk_button_new_with_label("Sing Up");
	g_signal_connect(buttonSingUp, "clicked", G_CALLBACK(on_button_sing_up), entryData);
	gtk_box_append(GTK_BOX(boxAuth), buttonSingUp);

	// Отображение окна и всех элементов
	gtk_widget_show(window);
}

static void
activate (GtkWidget *widget)
{
	window = gtk_application_window_new (app);
	gtk_window_set_title (GTK_WINDOW (window), "Mamaq Messenger");
	gtk_window_set_default_size (GTK_WINDOW (window), 200, 200);
	
	auth(widget);
}

int
main (int    argc,
      char **argv)
{
	//GtkApplication *app;
	int status;
	
	conn = PQconnectdb("host=127.0.0.1 dbname=cpp_db user=postgres password=DB_PASSWORD");
    
    EntryData *entryData = (EntryData*)malloc(sizeof(EntryData));
    
	glod_id_dialog = const_cast<char*>("1");

	app = gtk_application_new (NULL, G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
	status = g_application_run (G_APPLICATION (app), argc, argv);
	
	g_object_unref (app);

	return status;
}
