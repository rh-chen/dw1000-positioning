#include <Wire.h>
#include <SPI.h>

#include <DW1000.h>

#define PIN_IRQ  2
#define PIN_RST  9
#define PIN_SS  SS

/* Edit tagId */
const uint16_t tagId = 1;
const uint16_t networkId = 10;
#define ADDR_SIZE 2

#define ID_NONE 0

#define NUM_ANCHORS 5

#define CMD_NONE      0
#define CMD_SCAN      1
#define CMD_TYPE_NONE 2
#define CMD_TYPE_ID   3
#define CMD_TYPE_DIST 4

#define STATE_IDLE          0
#define STATE_SCAN          1
#define STATE_PONG          2
#define STATE_ROUNDROBIN    3
#define STATE_POLL          4
#define STATE_POLLACK       5
#define STATE_RANGE         6
#define STATE_RANGEREPORT   7

#define TYPE_NONE 0
#define TYPE_ID   1
#define TYPE_DIST 2

#define FTYPE_PING        0
#define FTYPE_PONG        1
#define FTYPE_POLL        2
#define FTYPE_POLLACK     3
#define FTYPE_RANGE       4
#define FTYPE_RANGEREPORT 5

#define FRAME_LEN 20

#warning "TODO: Temporary values"
#define PONG_TIMEOUT_MS        100
#define POLLACK_TIMEOUT_MS      10
#define RANGEREPORT_TIMEOUT_MS  10
#define REPLY_DELAY_MS           3

char cmd;
char state;
char type;

unsigned char num_anchors = 0;
unsigned char idx_anchor = 0;
uint16_t anchorId[NUM_ANCHORS] = {ID_NONE, ID_NONE, ID_NONE, ID_NONE, ID_NONE};
float distance[NUM_ANCHORS] = {0, 0, 0, 0, 0};

DW1000Time reply_delay = DW1000Time(REPLY_DELAY_MS, DW1000Time::MILLISECONDS);
DW1000Time timePollSent;
DW1000Time timePollReceived;
DW1000Time timePollAckSent;
DW1000Time timePollAckReceived;
DW1000Time timeRangeSent;
DW1000Time timeRangeReceived;

unsigned long curMillis;
unsigned long lastSent;

byte txBuffer[FRAME_LEN];
byte rxBuffer[FRAME_LEN];

boolean sentFrame;
boolean receivedFrame;

/***********************************************
 * I2C Raspberry Pi (master) - Arduino (slave) *
 ***********************************************/
void i2cReceiveEvent(int bytes) {
  if (!bytes) {
    return;
  }
  cmd = Wire.read();
  if (cmd == CMD_SCAN) {
    state = STATE_SCAN;
    return;
  }
  if (cmd == CMD_TYPE_NONE) {
    type = TYPE_NONE;
    return;
  }
  if (cmd == CMD_TYPE_ID) {
    type = TYPE_ID;
    return;
  }
  if (cmd == CMD_TYPE_DIST) {
    type = TYPE_DIST;
    return;
  }
}

void i2cRequestEvent() {
  if (state != STATE_IDLE || type == TYPE_NONE) {
    Wire.write(0);
    return;
  }
  if (type == TYPE_ID) {
    Wire.write((byte*)anchorId, 2 * NUM_ANCHORS);
    return;
  }
  if (type == TYPE_DIST) {
    Wire.write((byte*)distance, 4 * NUM_ANCHORS);
    return;
  }
}

void setupI2C() {
  // 7-bit addressing
  // ref: table 3, page 17, http://www.nxp.com/docs/en/user-guide/UM10204.pdf
  Wire.begin(0x04);
  Wire.onRequest(i2cRequestEvent);
  Wire.onReceive(i2cReceiveEvent);
  type = TYPE_NONE;
}

/*************************************
 * Arduino (master) - DW1000 (slave) *
 *************************************/
void spiReceiveEvent() {
  receivedFrame = true;
}

void spiSendEvent() {
  sentFrame = true;
}

void initDW1000Receiver() {
  DW1000.newReceive();
  DW1000.setDefaults();
  DW1000.receivePermanently(true);
  DW1000.startReceive();  
}

void setupDW1000() {
  DW1000.begin(PIN_IRQ, PIN_RST);
  DW1000.select(PIN_SS);
  DW1000.newConfiguration();
  DW1000.setDefaults();
  DW1000.setNetworkId(networkId);
  DW1000.setDeviceAddress(tagId);
  DW1000.enableMode(DW1000.MODE_LONGDATA_RANGE_LOWPOWER);
  DW1000.commitConfiguration();

  DW1000.attachSentHandler(spiSendEvent);
  DW1000.attachReceivedHandler(spiReceiveEvent);

  initDW1000Receiver();
}

void transmitPing() {
  #warning "TODO: implement"
  DW1000.newTransmit();
  DW1000.setDefaults();
  txBuffer[0] = FTYPE_PING;
  memcpy(txBuffer + 1, &tagId, ADDR_SIZE);
  DW1000.setData(txBuffer, FRAME_LEN);
  DW1000.startTransmit();
}

void transmitPoll() {
  DW1000.newTransmit();
  DW1000.setDefaults();
  txBuffer[0] = FTYPE_POLL;
  memcpy(txBuffer + 1, &tagId, ADDR_SIZE);
  memcpy(txBuffer + 3, &anchorId[idx_anchor], ADDR_SIZE);
  DW1000.setData(txBuffer, FRAME_LEN);
  DW1000.startTransmit();
}

void transmitRange() {
  DW1000.newTransmit();
  DW1000.setDefaults();
  txBuffer[0] = FTYPE_RANGE;
  memcpy(txBuffer + 1, &tagId, ADDR_SIZE);
  memcpy(txBuffer + 3, &anchorId[idx_anchor], ADDR_SIZE);
  DW1000.setDelay(reply_delay);
  DW1000.setData(txBuffer, FRAME_LEN);
  DW1000.startTransmit();
}

void calculateRange() {
    // asymmetric two-way ranging (more computation intense, less error prone)
    DW1000Time round1 = (timePollAckReceived - timePollSent).wrap();
    DW1000Time reply1 = (timePollAckSent - timePollReceived).wrap();
    DW1000Time round2 = (timeRangeReceived - timePollAckSent).wrap();
    DW1000Time reply2 = (timeRangeSent - timePollAckReceived).wrap();
    DW1000Time tof = (round1 * round2 - reply1 * reply2) / (round1 + round2 + reply1 + reply2);
    distance[idx_anchor] = tof.getAsMeters();
}

/********
 * Main *
 ********/

void setup() {
  state = STATE_IDLE;
  setupI2C();
  setupDW1000();
}

void loop() {
  curMillis = millis();
  if (sentFrame) {
    sentFrame = false;
    if (txBuffer[0] == FTYPE_POLL) {
      DW1000.getTransmitTimestamp(timePollSent);
      return;
    }
    if (txBuffer[0] == FTYPE_RANGE) {
      DW1000.getTransmitTimestamp(timeRangeSent);
      return;
    }
  }
  if (state == STATE_SCAN) {
    for (idx_anchor = 0; idx_anchor < NUM_ANCHORS; idx_anchor++) {
      anchorId[idx_anchor] = ID_NONE;
    }
    transmitPing();
    state = STATE_PONG;
    num_anchors = 0;
    lastSent = millis();
    return;
  }
  if (state == STATE_PONG) {
    if (curMillis - lastSent > PONG_TIMEOUT_MS) {
      if (!num_anchors) {
        state = STATE_IDLE;
      } else {
        state = STATE_ROUNDROBIN;
        idx_anchor = 0;
      }
      return;
    }
    if (receivedFrame) {
      receivedFrame = false;
      DW1000.getData(rxBuffer, FRAME_LEN);
      if (rxBuffer[0] != FTYPE_PONG) {
        return;
      }
      if (memcpy(rxBuffer + 3, &tagId, ADDR_SIZE)) {
        return;
      }
      memcpy(&anchorId[idx_anchor], rxBuffer + 1, ADDR_SIZE);
      num_anchors++;
    }
    return;
  }
  if (state == STATE_ROUNDROBIN) {
    if (idx_anchor < num_anchors) {
      transmitPoll();
      state = STATE_POLLACK;
      lastSent = millis();
    } else {
      state = STATE_IDLE;
    }
    return;
  }
  if (state == STATE_POLLACK) {
    if (curMillis - lastSent > POLLACK_TIMEOUT_MS) {
      state = STATE_ROUNDROBIN;
      idx_anchor++;
      return;
    }
    if (receivedFrame) {
      receivedFrame = false;
      DW1000.getData(rxBuffer, FRAME_LEN);
      if (rxBuffer[0] != FTYPE_POLLACK) {
        return;
      }
      if (memcpy(rxBuffer + 1, &anchorId[idx_anchor], ADDR_SIZE)) {
        return;
      }
      if (memcpy(rxBuffer + 3, &tagId, ADDR_SIZE)) {
        return;
      }
      transmitRange();
      state = STATE_RANGEREPORT;
      lastSent = millis();
    }
    return;
  }
  if (state == STATE_RANGEREPORT) {
    if (curMillis - lastSent > RANGEREPORT_TIMEOUT_MS) {
      state = STATE_ROUNDROBIN;
      idx_anchor++;
      return;
    }
    if (receivedFrame) {
      receivedFrame = false;
      DW1000.getData(rxBuffer, FRAME_LEN);
      if (rxBuffer[0] != FTYPE_RANGEREPORT) {
        return;
      }
      if (memcpy(rxBuffer + 1, &anchorId[idx_anchor], ADDR_SIZE)) {
        return;
      }
      if (memcpy(rxBuffer + 3, &tagId, ADDR_SIZE)) {
        return;
      }
      timePollReceived.setTimestamp(rxBuffer + 5);
      timePollAckSent.setTimestamp(rxBuffer + 10);
      timeRangeReceived.setTimestamp(rxBuffer + 15);
      calculateRange();
      state = STATE_ROUNDROBIN;
    }
    return;
  }
}