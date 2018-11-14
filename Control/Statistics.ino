/*
 * Графики, история, статистика
 * Автор vad711, vad7@yahoo.com
 *
 * "Народный контроллер" для тепловых насосов.
 * Данное програмноое обеспечение предназначено для управления
 * различными типами тепловых насосов для отопления и ГВС.
 *
 * This file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 */
#include "Statistics.h"
#include "HeatPump.h"
#include "SdFat.h"

#define temp_initbuf Socket[0].outBuf
const char format_date[] = "%04d%02d%02d";
const char format_datetime[] = "%04d%02d%02d%02d%02d%02d";
#define format_date_size 8
char filename[sizeof(stats_file_start)-1 + 4 + sizeof(stats_file_ext)];

// what: 0 - Stats, 1 - History, Return: OK or Error
int8_t Statistics::CreateOpenFile(uint8_t what)
{
	if(!HP.get_fSD()) return ERR_SD_INIT;
	if(what == ID_HISTORY) {
		HistoryBlockStart = 0;
		HistoryCurrentPos = 0;
		strcpy(filename, history_file_start);
	} else {
		BlockStart = 0;
		CurrentPos = 0;
		strcpy(filename, stats_file_start);
	}
	_itoa(year, filename);
	strcat(filename, stats_file_ext);
	journal.jprintf(" File: %s ", filename);
	SPI_switchSD();
	uint8_t newfile = 0;
	if(!StatsFile.open(filename, O_READ)) {
		uint16_t days = month == 1 ? 366 : (12 - month + 1) * 31;
		if(!StatsFile.createContiguous(filename, what ? HISTORY_MAX_FILE_SIZE(days) : STATS_MAX_FILE_SIZE(days))) {
			Error("create", what);
			return ERR_SD_WRITE;
		} else {
			StatsFile.timestamp(T_CREATE | T_ACCESS | T_WRITE, rtcSAM3X8.get_years(), rtcSAM3X8.get_months(), rtcSAM3X8.get_days(), rtcSAM3X8.get_hours(), rtcSAM3X8.get_minutes(), rtcSAM3X8.get_seconds());
		}
		newfile = 1;
	}
	if(!StatsFile.contiguousRange(what ? &HistoryBlockStart : &BlockStart, what ? &HistoryBlockEnd : &BlockEnd)) {
		journal.jprintf("Error get blocks %s!\n", filename);
	} else {
		journal.jprintf("[%u..%u]", what ? HistoryBlockStart : BlockStart, what ? HistoryBlockEnd : BlockEnd);
		if(newfile) {
			journal.jprintf(" - Create ");
			uint32_t b;
			if(what) {
				b = HistoryCurrentBlock = HistoryBlockStart;
				memset(history_buffer, 0, SD_BLOCK);
			} else {
				b = CurrentBlock = BlockStart;
				memset(stats_buffer, 0, SD_BLOCK);
			}
			for(; b <= (what ? HistoryBlockEnd : BlockEnd); b++) {
				WDT_Restart(WDT);
				if(!card.card()->writeBlock(b, what ? (uint8_t*)history_buffer : (uint8_t*)stats_buffer)) {
					Error("empty", what);
					goto xError;
				}
				if((b & 0x7FF) == 0) journal.jprintf("."); // каждый 1Мб
				if((b & 0xF) == 0) _delay(1); // время другим задачам
			}
			journal.jprintf("Ok\n");
			return OK;
		} else if(!FindEndPosition(what)) {
			journal.jprintf(" Endpos not found!\n");
		} else {
			return OK;
		}
	}
xError:
	StatsFile.close();
	return ERR_SD_WRITE;
}

boolean Statistics::FindEndPosition(uint8_t what)
{
	uint8_t *buffer, *pos = NULL;
	uint32_t bst, bend, cur;
	if(what) {
		bst = HistoryBlockStart;
		bend = HistoryBlockEnd;
		buffer = history_buffer;
	} else {
		bst = BlockStart;
		bend = BlockEnd;
		buffer = stats_buffer;
	}
	while(bst <= bend) {
		WDT_Restart(WDT);
		cur = bst + (bend - bst) / 2;
		if(!card.card()->readBlock(cur, buffer)) {
			Error("FindPos", what);
			break;
		}
		if(*buffer) {
			if((pos = (uint8_t*)memchr(buffer, 0, SD_BLOCK))) break;
			bst = cur + 1;
			if(bst > bend) {
				if(bend < (what ? HistoryBlockEnd : BlockEnd)) bend++; else break; // file overflow
			}
		} else if(cur == bst) { // empty
			pos = buffer;
			break;
		} else bend = cur - 1;
	}
	if(pos == NULL) return false;
	if(what) {
		HistoryCurrentBlock = cur;
		HistoryCurrentPos = pos - buffer;
	} else {
		CurrentBlock = cur;
		CurrentPos = pos - buffer;
	}
//#ifdef DEBUG_MODWORK
	journal.jprintf(" End pos: %u/%u\n", cur, pos - buffer);
//#endif
	return true;
}

void Statistics::Error(const char *text, uint8_t what)
{
	journal.jprintf(" %s Error %s (%d,%d)!\n", what ? "History" : "Stats", text, card.cardErrorCode(), card.cardErrorData());
}

void Statistics::Init(uint8_t newyear)
{
	if(!newyear) Reset();
	HistoryCurrentBlock = 0;
	year = rtcSAM3X8.get_years();
#ifdef STATS_DO_NOT_SAVE
	return;
#endif
	if(!HP.get_fSD()) {
		journal.jprintf(" No SD card - statistics will not be saved!\n");
		return;
	}
	if(CreateOpenFile(ID_STATS) == OK) {
		if(!newyear) { // read last stats record
			int32_t pos = (CurrentBlock - BlockStart) * SD_BLOCK + CurrentPos - 1;
			uint8_t b;
			while(--pos >= 0) {
				if(!StatsFile.seekSet(pos)) {
					Error("seek", ID_STATS);
					break;
				}
				if(!StatsFile.read(&b, 1)) {
					Error("readb", ID_STATS);
					break;
				}
				if(b == '\n' || pos == 0) {
					if(pos) pos++;
					if(!StatsFile.read(temp_initbuf, STATS_MAX_RECORD_LEN)) {
						Error("readl", ID_STATS);
						break;
					}
					m_snprintf(temp_initbuf + STATS_MAX_RECORD_LEN, 16, format_date, year, month, day);
					if(memcmp(temp_initbuf, temp_initbuf + STATS_MAX_RECORD_LEN, format_date_size) == 0) { // date the same
						CurrentBlock = BlockStart + pos / SD_BLOCK;
						CurrentPos = pos % SD_BLOCK;
						temp_initbuf[format_date_size] = '\0';
						journal.printf(" %s restored at %d\n", temp_initbuf, pos);
						if(!card.card()->readBlock(CurrentBlock, (uint8_t*)stats_buffer)) {
							Error("readp", ID_STATS);
						} else {
							memcpy(temp_initbuf, stats_buffer + CurrentPos, SD_BLOCK - CurrentPos);
							temp_initbuf[STATS_MAX_RECORD_LEN] = '\0';
							char *p = temp_initbuf + format_date_size;
							if((p = strchr(p, '\n'))) *p = '\0';
							p = temp_initbuf + format_date_size;
							while((p = strchr(p, ';'))) *p++ = '\0';
							p = temp_initbuf + format_date_size;
							for(uint8_t i = 0; i < sizeof(Stats_data) / sizeof(Stats_data[0]); i++) {
								float val = my_atof(++p);
								if(val != ATOF_ERROR) {
									switch(Stats_data[i].object) {
									case STATS_OBJ_Temp:
									case STATS_OBJ_Press:
										Stats_data[i].value = val * 100;
										break;
									case STATS_OBJ_Voltage:
										Stats_data[i].value = val;
										break;
									case STATS_OBJ_Power:
										switch(Stats_data[i].type) {
										case STATS_TYPE_SUM:
										case STATS_TYPE_AVG:
											Stats_data[i].value = val * 1000000;
											break;
										default:
											Stats_data[i].value = val * 1000;
										}
										break;
									case STATS_OBJ_COP:
										Stats_data[i].value = val * 100;
										break;
									default:
										if(Stats_data[i].type == STATS_TYPE_TIME) Stats_data[i].value = val * 60000;
										break;
									}
									if(Stats_data[i].value == 0) {
										switch(Stats_data[i].type) {
										case STATS_TYPE_MIN:
											Stats_data[i].value = MAX_INT32_VALUE;
											break;
										case STATS_TYPE_MAX:
											Stats_data[i].value = MIN_INT32_VALUE;
											break;
										}
									}
									if(Stats_data[i].when == STATS_WHEN_WORKD && Stats_data[i].value) counts_work = 1;
									if((p = (char*)memchr(p, '\0', STATS_MAX_RECORD_LEN)) == NULL) break;
								}
							}
							counts = 1;
						}
					}
					break;
				}
			}
		}
		StatsFile.close();
	}
}

void Statistics::CheckCreateNewFile()
{
	uint8_t sem = 0;
	if(year == 0) {
		if(!(sem = SemaphoreTake(xWebThreadSemaphore, 0))) return;
		Init(1);
	}
	if(GETBIT(HP.Option.flags, fHistory) && HistoryCurrentBlock == 0) { // Init History
		if(!sem && !(sem = SemaphoreTake(xWebThreadSemaphore, 0))) return;
		if(CreateOpenFile(ID_HISTORY) == OK) {
			StatsFile.close();
		} else SETBIT0(HP.Option.flags, fHistory); // При ошибке выключаем опцию сохранения истории!
	}
	if(sem) SemaphoreGive(xWebThreadSemaphore);
}

// Сбросить накопленные промежуточные значения
void Statistics::Reset()
{
	for(uint8_t i = 0; i < sizeof(Stats_data) / sizeof(Stats_data[0]); i++) {
		switch(Stats_data[i].type){
		case STATS_TYPE_MIN:
			Stats_data[i].value = MAX_INT32_VALUE;
			break;
		case STATS_TYPE_MAX:
			Stats_data[i].value = MIN_INT32_VALUE;
			break;
		default:
			Stats_data[i].value = 0;
		}
	}
	counts = 0;
	counts_work = 0;
	compressor_on_timer = 0;
	day = rtcSAM3X8.get_days();
	month = rtcSAM3X8.get_months();
	previous = millis();
}

// Обновить статистику, вызывается часто, раз в TIME_READ_SENSOR
void Statistics::Update()
{
	if(year == 0 || HP.get_testMode() != NORMAL) return; // waiting to switch a next year
	uint32_t tm = millis() - previous;
	previous = millis();
	if(rtcSAM3X8.get_days() != day) {
		if(SaveStats(2) == OK) {
			Reset();
			if(year != rtcSAM3X8.get_years()) year = 0; // waiting to switch a next year
		}
	}
	int32_t newval = 0;
	boolean compressor_on = HP.is_compressor_on();
	if(compressor_on) {
		compressor_on_timer += tm;
		if(compressor_on_timer >= STATS_WORKD_TIME) counts_work++;
	} else compressor_on_timer = 0;
	for(uint8_t i = 0; i < sizeof(Stats_data) / sizeof(Stats_data[0]); i++) {
		if(Stats_data[i].when == STATS_WHEN_WORKD && compressor_on_timer < STATS_WORKD_TIME) continue;
		switch(Stats_data[i].object) {
		case STATS_OBJ_Temp:
			newval = HP.sTemp[Stats_data[i].number].get_Temp();
			break;
		case STATS_OBJ_Press:
			newval = HP.sADC[Stats_data[i].number].get_Press();
			break;
		case STATS_OBJ_Voltage:
		    #ifdef USE_ELECTROMETER_SDM
			newval = HP.dSDM.get_Voltage();
			#endif
			break;
		case STATS_OBJ_Power:
			if(Stats_data[i].number == OBJ_powerCO) { // Система отопления
				newval = HP.powerCO; // Вт
			} else if(Stats_data[i].number == OBJ_powerGEO) { // Геоконтур
				newval = HP.powerGEO; // Вт
			} else if(Stats_data[i].number == OBJ_power220) { // Геоконтур
				newval = HP.power220; // Вт
			} else continue;
			switch(Stats_data[i].type) {
			case STATS_TYPE_SUM:
			case STATS_TYPE_AVG:
				newval = newval * tm / 3600; // в мВт
				if(Stats_data[i].number == OBJ_powerCO) motohour_OUT_work += newval; // для motoHour
			}
			break;
		case STATS_OBJ_COP:
			if(Stats_data[i].number == OBJ_COP_Compressor) {
				newval = HP.COP;
			} else if(Stats_data[i].number == OBJ_COP_Full) {
#ifdef STATS_SKIP_COP_WHEN_RELAY_ON
				if(HP.dRelay[STATS_SKIP_COP_WHEN_RELAY_ON].get_Relay()) continue;
#endif
				newval = HP.fullCOP;
			}
			if(newval == 0) continue;
			break;
		case STATS_OBJ_Sun:
			if(!GETBIT(HP.flags, fHP_SunActive)) continue;
			break;
		case STATS_OBJ_Compressor:
			if(!compressor_on) continue;
			break;
		}
		switch(Stats_data[i].type){
		case STATS_TYPE_MIN:
			if(newval < Stats_data[i].value) Stats_data[i].value = newval;
			break;
		case STATS_TYPE_MAX:
			if(newval > Stats_data[i].value) Stats_data[i].value = newval;
			break;
		case STATS_TYPE_AVG:
		case STATS_TYPE_SUM:
			Stats_data[i].value += newval;
			break;
		case STATS_TYPE_TIME:
			Stats_data[i].value += tm;
			break;
		}
	}
	counts++;
//	for(uint8_t i = 0; i < sizeof(Stats_data) / sizeof(Stats_data[0]); i++) journal.jprintf("%d=%d, ", i, Stats_data[i].value); journal.jprintf("\n");
}

void Statistics::StatsFieldHeader(char *ret, uint8_t i, uint8_t flag)
{
	if(flag && Stats_data[i].type == STATS_TYPE_TIME) strcat(ret, "M"); // ось часы
	switch(Stats_data[i].object) {
	case STATS_OBJ_Temp:
		if(flag) strcat(ret, "T"); // ось температур
		strcat(ret, HP.sTemp[Stats_data[i].number].get_note());
		break;
	case STATS_OBJ_Press:
		if(flag) strcat(ret, "P"); // ось давление
		strcat(ret, HP.sADC[Stats_data[i].number].get_note());
		break;
	case STATS_OBJ_Voltage:
		if(flag) strcat(ret, "V"); // ось напряжение
		strcat(ret, "Напряжение, V");
		break;
	case STATS_OBJ_Power:
		if(flag) strcat(ret, "W"); // ось мощность
		if(Stats_data[i].number == OBJ_powerCO) { // Система отопления
			strcat(ret, "Выработано, кВтч"); // хранится в Вт
		} else if(Stats_data[i].number == OBJ_powerGEO) { // Геоконтур
			strcat(ret, "Геоконтур, кВтч"); // хранится в Вт
		} else if(Stats_data[i].number == OBJ_power220) { // Геоконтур
			strcat(ret, "Потребление, кВтч"); // хранится в Вт
		}
		break;
	case STATS_OBJ_COP:
		if(flag) strcat(ret, "C"); // ось COP
		if(Stats_data[i].number == OBJ_COP_Compressor) {
			strcat(ret, "КОП");
		} else if(Stats_data[i].number == OBJ_COP_Full) {
			strcat(ret, "Полный КОП");
		}
		break;
	case STATS_OBJ_Compressor:
		strcat(ret, "Моточасы, м");
		break;
	case STATS_OBJ_Sun:
		strcat(ret, "СК время, м");
		break;
	default: strcat(ret, "?");
	}
	switch(Stats_data[i].type) {
	case STATS_TYPE_MIN:
		strcat(ret, " - Мин");
		break;
	case STATS_TYPE_MAX:
		strcat(ret, " - Макс");
		break;
	case STATS_TYPE_AVG:
		strcat(ret, " - Сред");
		break;
	}
	if(Stats_data[i].when == STATS_WHEN_WORKD) strcat(ret, "(W)");
}

// Возвращает файл с заголовками полей
void Statistics::StatsFileHeader(char *ret, uint8_t flag)
{
	if(!flag) strcat(ret, "Дата;");
	for(uint8_t i = 0; i < sizeof(Stats_data) / sizeof(Stats_data[0]); i++) {
		if(i > 0) strcat(ret, ";");
		StatsFieldHeader(ret, i, flag);
	}
	strcat(ret, "\n");
}

void Statistics::StatsFieldString(char **ret, uint8_t i)
{
	int32_t val = Stats_data[i].type == STATS_TYPE_AVG ? Stats_data[i].value / (Stats_data[i].when == STATS_WHEN_WORKD ? counts_work : counts) : Stats_data[i].value;
	if(val == MIN_INT32_VALUE || val == MAX_INT32_VALUE) val = 0;
	switch(Stats_data[i].object) {
	case STATS_OBJ_Temp:					// C
	case STATS_OBJ_Press: 					// bar
		int_to_dec_str(val, 100, ret, 1);
		break;
	case STATS_OBJ_Voltage:					// V
		int_to_dec_str(val, 1, ret, 0);
		break;
	case STATS_OBJ_Power:					// кВт*ч
		switch(Stats_data[i].type) {
		case STATS_TYPE_SUM:
		case STATS_TYPE_AVG:
			int_to_dec_str(val, 1000000, ret, 3);
			break;
		default:
			int_to_dec_str(val, 1000, ret, 3);
		}
		break;
	case STATS_OBJ_COP:
		int_to_dec_str(val, 100, ret, 2);
		break;
	default:
		if(Stats_data[i].type == STATS_TYPE_TIME) int_to_dec_str(val, 60000, ret, 1);  // минуты;
		break;
	}
}

// Строка со значениями за день (разделитель ";"), при запуске не из Update() возможны неверные данные!
inline void Statistics::StatsFileString(char *ret)
{
	ret += m_snprintf(ret, 20, format_date, year, month, day);
	for(uint8_t i = 0; i < sizeof(Stats_data) / sizeof(Stats_data[0]); i++) {
		*ret++ = ';';
		StatsFieldString(&ret, i);
	}
	*ret = '\n'; *(ret+1) = '\0';
}

void Statistics::StatsWebTable(char *ret)
{
	for(uint8_t i = 0; i < sizeof(Stats_data) / sizeof(Stats_data[0]); i++) {
		StatsFieldHeader(ret, i, 0);
		ret += m_strlen(ret);
		*ret++ = '|';
		StatsFieldString(&ret, i);
		strcat(ret, ";");
	}
}

// Return: OK, 1 - not found, >2 - error. Network is active
void Statistics::SendFileData(uint8_t thread, SdFile *File, char *filename)
{
	fname_t fname;
	SPI_switchSD();
	if(!File->opens(filename, O_READ, &fname)) {
		SPI_switchW5200();
		sendConstRTOS(thread, HEADER_FILE_NOT_FOUND);
		return;
	}
	uint32_t bst, bend;
	if(!File->contiguousRange(&bst, &bend)) {
		journal.jprintf(" Error get blocks %s\n", filename);
		File->close();
		return;
	}
	File->close();
	uint32_t readed = 0;
	for(uint32_t i = bst; i <= bend; i++) {
		SPI_switchSD();
		if(i == CurrentBlock) {
			memcpy((uint8_t*)Socket[thread].outBuf + readed, stats_buffer, SD_BLOCK);
		} else if(i == HistoryCurrentBlock) {
			memcpy((uint8_t*)Socket[thread].outBuf + readed, history_buffer, SD_BLOCK);
		} else if(!card.card()->readBlock(i, (uint8_t*)Socket[thread].outBuf + readed)) {
			Error("read data", ID_STATS);
			break;
		}
		if(Socket[thread].outBuf[readed + SD_BLOCK - 1] == 0) {  // end of data
			readed = (uint8_t*)memchr((uint8_t*)Socket[thread].outBuf + readed, 0, SD_BLOCK) - (uint8_t*)Socket[thread].outBuf;
			if(readed == 0) break;
			bend = 0;
		} else {
			readed += SD_BLOCK;
			if(readed <= W5200_MAX_LEN - SD_BLOCK) continue;
		}
		if(sendPacketRTOS(thread, (byte*)Socket[thread].outBuf, readed, 0) != readed) {
			journal.jprintf(" Error send %s\n", filename);
			break;
		}
		readed = 0;
	}
}

// Return: OK, 1 - not found, >2 - error. Network is active. Date format: "yyyymmdd;\0"
void Statistics::SendFileDataByPeriod(uint8_t thread, SdFile *File, char *Prefix, char *TimeStart, char *TimeEnd)
{
	strncat(Prefix, TimeStart, 4); // year
	strcat(Prefix, stats_file_ext);
	fname_t fname;
	SPI_switchSD();
	if(!File->opens(Prefix, O_READ, &fname)) {
		SPI_switchW5200();
		sendConstRTOS(thread, HEADER_FILE_NOT_FOUND);
		return;
	}
	uint32_t cur, bst, bend, bendfile;
	if(!File->contiguousRange(&bst, &bend)) {
		journal.jprintf(" Error get blocks %s\n", filename);
		File->close();
		return;
	}
	File->close();
	bendfile = bend;
	char* buffer = Socket[thread].outBuf;
	while(bst <= bend) {
		WDT_Restart(WDT);
		cur = bst + (bend - bst) / 2;
		if(!card.card()->readBlock(cur, (uint8_t*)buffer)) {
			Error("FindPos", ID_HISTORY);
			break;
		}
		if(*buffer) {
			char *pos = (char*)memchr(buffer, '\n', SD_BLOCK);
			if(pos == NULL) {  // garbage
				bst = 0;
				break;
			}
			if(*++pos == '\0') goto xGoDown;
			int8_t cmp = strncmp(pos, TimeStart, m_strlen(TimeStart));
			if(cmp == 0) break;
			if(cmp > 0) goto xGoDown;
			bst = cur + 1;
			if(bst > bend) {
				if(bend < bendfile) bend++;
				else { // file overflow
					bst = cur;
					break;
				}
			}
		} else {
xGoDown:	if(cur == bst) { // empty
				break;
			} else bend = cur - 1;
		}
	}
	uint32_t readed = 0;
	for(uint32_t i = bst; i <= bendfile; i++) {
		SPI_switchSD();
		if(i == CurrentBlock) {
			memcpy((uint8_t*)Socket[thread].outBuf + readed, stats_buffer, SD_BLOCK);
		} else if(i == HistoryCurrentBlock) {
			memcpy((uint8_t*)Socket[thread].outBuf + readed, history_buffer, SD_BLOCK);
		} else if(!card.card()->readBlock(i, (uint8_t*)Socket[thread].outBuf + readed)) {
			Error("read data", ID_STATS);
			break;
		}
		if(Socket[thread].outBuf[readed + SD_BLOCK - 1] == 0) {  // end of data
			readed = (uint8_t*)memchr((uint8_t*)Socket[thread].outBuf + readed, 0, SD_BLOCK) - (uint8_t*)Socket[thread].outBuf;
			if(readed == 0) break;
			bendfile = 0;
		} else {
			char *pos = (char*)memchr(Socket[thread].outBuf + readed, '\n', SD_BLOCK);
			readed += SD_BLOCK;
			if(pos) {
				if(strncmp(pos + 1, TimeEnd, m_strlen(TimeEnd)) > 0) bendfile = 0; // stop
				else if(readed <= W5200_MAX_LEN - SD_BLOCK) continue;
			} else bendfile = 0;
		}
		if(sendPacketRTOS(thread, (byte*)Socket[thread].outBuf, readed, 0) != readed) {
			journal.jprintf(" Error send %s\n", filename);
			break;
		}
		readed = 0;
	}
}

// Записать статистику на SD, 0 - только записать, 1 - только записать c веба, 2 - новый день
// Return: OK или Ошибка
int8_t Statistics::SaveStats(uint8_t newday)
{
#ifdef STATS_DO_NOT_SAVE
	return OK;
#endif
	if(!HP.get_fSD() || CurrentBlock == 0) return OK;
	char *rbuf = (char*) malloc(STATS_MAX_RECORD_LEN);
	if(rbuf == NULL) {
		Error("memory low", ID_STATS);
		return ERR_OUT_OF_MEMORY;
	}
	int8_t retval = OK;
	StatsFileString(rbuf);
	uint16_t lensav, len = m_strlen(rbuf) + 1;
	memcpy(stats_buffer + CurrentPos, rbuf, lensav = SD_BLOCK - CurrentPos < len ? SD_BLOCK - CurrentPos : len);
#ifdef STATS_USE_BUFFER_FOR_SAVING
	if(newday < 2 || lensav != len) { // save when there is no space in buffer
#endif
		if(newday != 1 && SemaphoreTake(xWebThreadSemaphore, newday == 0 ? W5200_TIME_WAIT : 0) == pdFALSE) {
			retval = ERR_CONFIG;
			free(rbuf);
			return retval;
		}
		SPI_switchSD();
		if(!card.card()->writeBlock(CurrentBlock, (uint8_t*)stats_buffer)) {
			Error("save", ID_STATS);
			// to do - reinit card but in other task
			//if(card.cardErrorCode() > SD_CARD_ERROR_NONE && card.cardErrorCode() < SD_CARD_ERROR_READ && card.cardErrorData() == 255) { // reinit card
			//	if(card.begin(PIN_SPI_CS_SD, SD_SCK_MHZ(SD_CLOCK))) goto xContinue;
			//	else journal.jprintf("Reinit SD card failed!\n");
			//}
			retval = ERR_SD_WRITE;
		} else if(lensav != len){ // next block
			if(CurrentBlock >= BlockEnd) {
				journal.jprintf("Stats file size exceeded!\n"); // to do: increase file
				retval = ERR_SD_WRITE;
			} else {
				memset(stats_buffer, 0, SD_BLOCK);
				memcpy(stats_buffer, rbuf + lensav, len - lensav);
				if(!card.card()->writeBlock(CurrentBlock + 1, (uint8_t*)stats_buffer)) {
					Error("save 2", ID_STATS);
					retval = ERR_SD_WRITE;
				} else if(newday == 2) { // new day
					if(CurrentBlock >= BlockEnd) {
						Error("File Overflow", ID_STATS);
						retval = ERR_CONFIG;
					} else CurrentBlock++;
					CurrentPos = len - lensav - 1;
				} else { // reread current block
					if(!card.card()->readBlock(CurrentBlock, (uint8_t*)stats_buffer)) {
						Error("read", ID_STATS);
						retval = ERR_SD_READ;
					}
				}
			}
		} else if(newday == 2) CurrentPos += lensav - 1; // new day
	    if(newday != 1) SemaphoreGive(xWebThreadSemaphore);
#ifdef STATS_USE_BUFFER_FOR_SAVING
	} else CurrentPos += lensav - 1; // new day
#endif
	free(rbuf);
	return retval;
}

// Return: OK или Ошибка
int8_t Statistics::SaveHistory(uint8_t from_web)
{
#ifdef STATS_DO_NOT_SAVE
	return OK;
#endif
	if(!GETBIT(HP.Option.flags, fHistory) || !HP.get_fSD() || HistoryCurrentBlock == 0) return OK;
	if(!from_web && SemaphoreTake(xWebThreadSemaphore, W5200_TIME_WAIT) == pdFALSE) return ERR_CONFIG;
	int8_t retval = OK;
	SPI_switchSD();
	if(!card.card()->writeBlock(HistoryCurrentBlock, (uint8_t*)history_buffer)) {
		Error("save", ID_STATS);
		retval = ERR_SD_WRITE;
	}
	if(!from_web) SemaphoreGive(xWebThreadSemaphore);
	return retval;
}

// Логирование параметров работы ТН, раз в 1 минуту
void Statistics::History()
{
	if(!GETBIT(HP.Option.flags, fHistory) || HP.get_testMode() != NORMAL) return;
	char *mbuf = (char*) malloc(HISTORY_MAX_RECORD_LEN);
	if(mbuf == NULL) {
		Error("memory low", ID_HISTORY);
	}
	char *buf = mbuf;
	buf += m_snprintf(buf, 20, format_datetime, year, month, day, rtcSAM3X8.get_hours(), rtcSAM3X8.get_minutes(), rtcSAM3X8.get_seconds());
	for(uint8_t i = 0; i < sizeof(HistorySetup) / sizeof(HistorySetup[0]); i++) {
		*buf++ = ';';
		switch(HistorySetup[i].object) {
		case STATS_OBJ_Temp:		// C
			int_to_dec_str(HP.sTemp[HistorySetup[i].number].get_Temp(), 100, &buf, 1);
			break;
		case STATS_OBJ_Press:		// bar
			int_to_dec_str(HP.sADC[HistorySetup[i].number].get_Press(), 100, &buf, 1);
			break;
		case STATS_OBJ_PressTemp:	// C
			int_to_dec_str(PressToTemp(HP.sADC[HistorySetup[i].number].get_Press(), HP.dEEV.get_typeFreon()), 100, &buf, 1);
			break;
		case STATS_OBJ_Flow:		// m3h
			int_to_dec_str(HP.sFrequency[HistorySetup[i].number].get_Value(), 1000, &buf, 1);
			break;
#ifdef EEV_DEF
		case STATS_OBJ_EEV:
			switch(HistorySetup[i].number) {
			case STATS_EEV_Steps:
				int_to_dec_str(HP.dEEV.get_EEV(), 1, &buf, 0);
				break;				
			case STATS_EEV_Percent:
				int_to_dec_str(HP.dEEV.get_EEV_percent(), 100, &buf, 1);
				break;
			case STATS_EEV_OverHeat:
				int_to_dec_str(HP.dEEV.get_Overheat(), 100, &buf, 1);
				break;
			case STATS_EEV_OverCool:
				int_to_dec_str(HP.get_overcool(), 100, &buf, 1);
				break;
			}
			break;
#endif
		case STATS_OBJ_Compressor:
//			switch(HistorySetup[i].number) {
//			case OBJ_Freq:
				int_to_dec_str(HP.dFC.get_frequency(), 100, &buf, 1);
//				break;
//			}
			break;
		case STATS_OBJ_Power:
			switch(HistorySetup[i].number) {
			case OBJ_power220:
				int_to_dec_str(HP.power220, 1000, &buf, 3);
				break;
			case OBJ_powerCO:
				int_to_dec_str(HP.powerCO, 1000, &buf, 1);
				break;
			}
			break;
		case STATS_OBJ_COP:
			int_to_dec_str(HP.fullCOP, 1000, &buf, 3);
			break;
		}
		if(buf > mbuf + HISTORY_MAX_RECORD_LEN - 8) {
			journal.jprintf("%s memory overflow(%d): %d, max: %d\n", "History", i, buf - mbuf, HISTORY_MAX_RECORD_LEN);
			break;
		}
	}
	*buf++ = '\n'; *buf = '\0';
	uint16_t lensav, len = buf - mbuf + 1;
	memcpy(history_buffer + HistoryCurrentPos, mbuf, lensav = SD_BLOCK - HistoryCurrentPos < len ? SD_BLOCK - HistoryCurrentPos : len);
	if(lensav != len) { // save when there is no space in buffer
		if(SaveHistory(0) == OK) {
			if(HistoryCurrentBlock >= HistoryBlockEnd) {
				Error("File Overflow", ID_HISTORY);
			} else HistoryCurrentBlock++;
			memset(history_buffer, 0, SD_BLOCK);
			memcpy(history_buffer, mbuf + lensav, HistoryCurrentPos = len - lensav - 1);
		}
	} else HistoryCurrentPos += lensav - 1;
	free(mbuf);
}

// Возвращает файл с заголовками полей
void Statistics::HistoryFileHeader(char *ret, uint8_t flag)
{
	if(!flag) strcat(ret, "Время;");
	for(uint8_t i = 0; i < sizeof(HistorySetup) / sizeof(HistorySetup[0]); i++) {
		if(i > 0) strcat(ret, ";");
		strcat(ret, HistorySetup[i].name);
	}
	strcat(ret, "\n");
}
