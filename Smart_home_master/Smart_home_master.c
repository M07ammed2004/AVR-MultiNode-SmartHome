#include <avr/io.h>
#include <util/delay.h>
#include <string.h>
#include <avr/eeprom.h>
#include <stdio.h>

#define F_CPU 8000000UL
#define BAUD 9600
#define MYUBRR (F_CPU/16/BAUD-1)

// LCD Connections
#define LCD_PORT PORTB
#define LCD_DDR DDRB
#define RS PD6
#define E PD7
#define D4 PB1
#define D5 PB2
#define D6 PB3
#define D7 PB4

// Keypad Connections
#define KEYPAD_PORT PORTA
#define KEYPAD_DDR DDRA
#define KEYPAD_PIN PINA

// Buzzer
#define BUZZER_PORT PORTB
#define BUZZER_DDR DDRB
#define BUZZER_PIN PB0

#define SERVO_DDR DDRB
#define SERVO_PORT PORTB


// Software UART Pins
#define SOFT_TX PD2
#define SOFT_RX PD3

// Floor definitions
#define GROUND_FLOOR 0
#define FIRST_FLOOR 1

// EEPROM password storage
char EEMEM saved_password[5] = "1234";
uint8_t EEMEM attempt_count = 0;
volatile uint8_t current_floor = GROUND_FLOOR;
volatile uint8_t show_sensor = 0;

// Global variables for menu system
volatile uint8_t current_menu = 0;
volatile unsigned long _delay_ms_counter = 0;
volatile uint8_t current_floor_menu = GROUND_FLOOR;

// Initialize EEPROM with default values if empty
void initialize_eeprom() {
    char current_pw[5];
    eeprom_read_block((void*)current_pw, (const void*)saved_password, 5);

    if(current_pw[0] == 0xFF) { // Check if EEPROM is empty
        char default_pw[5] = "0000";
        eeprom_update_block((const void*)default_pw, (void*)saved_password, 5);
        eeprom_update_byte(&attempt_count, 0);
    }
}
// Servo control functions (Added)
void PWM_init()
{
    // Set PD5 (OC1A) as output
    DDRD |= (1 << PD5);

    // Set Fast PWM, non-inverting mode, prescaler 8
    TCCR1A |= (1 << COM1A1) | (1 << WGM11);
    TCCR1B |= (1 << WGM12) | (1 << WGM13) | (1 << CS11);

    // Set TOP value for 20ms period (50Hz) - ICR1
    ICR1 = 19999; // For 16MHz clock, prescaler 8: 16e6/8 = 2e6; 2e6/50Hz=40000; ICR1=39999
}

void servo_set_angle(uint8_t angle)
{
    // 0 degree = 1ms pulse, 180 degree = 2ms pulse
    // OCR1A controls the pulse width; pulse width = (angle/180)*1ms + 1ms
    uint16_t pulse = ((angle * 1000) / 180) + 1000; // in microseconds
    OCR1A = pulse * 2; // Because 1 count = 0.5us (with our timer settings)
}


//void operate_door() {
//    servo_set_position(90); // Open door (90 degrees)
//    _delay_ms(3000);       // Keep door open for 3 seconds
//    servo_set_position(0); // Close door (0 degrees)
//}

// Hardware USART Initialization
void USART_Init(unsigned int ubrr) {
    UBRRH = (unsigned char)(ubrr>>8);
    UBRRL = (unsigned char)ubrr;
    UCSRB = (1<<RXEN)|(1<<TXEN);
    UCSRC = (1<<URSEL)|(1<<USBS)|(3<<UCSZ0);
}

// Hardware USART Transmit function
void USART_Transmit(char data) {
    while (!(UCSRA & (1<<UDRE)));
    UDR = data;
}

// Improved USART Receive with timeout
char USART_Receive() {
    unsigned int timeout = 0;
    while (!(UCSRA & (1<<RXC))) {
        if(timeout++ > 10000) return 0xFF; // Timeout after ~100ms
        _delay_us(10);
    }
    return UDR;
}

// Software UART Functions with error handling
void uart_tx_char(char c) {
    uint8_t i;
    PORTD &= ~(1 << SOFT_TX); // Start bit
    _delay_us(104);
    for (i = 0; i < 8; i++) {
        if (c & (1 << i))
            PORTD |= (1 << SOFT_TX);
        else
            PORTD &= ~(1 << SOFT_TX);
        _delay_us(104);
    }
    PORTD |= (1 << SOFT_TX); // Stop bit
    _delay_us(104);
}

char uart_rx_char() {
    char data = 0;
    unsigned int timeout = 0;

    // Wait for start bit with timeout
    while ((PIND & (1 << SOFT_RX))) {
        if(timeout++ > 10000) return 0xFF; // Timeout after ~100ms
        _delay_us(10);
    }

    _delay_us(104); // Middle of start bit
    for (uint8_t i = 0; i < 8; i++) {
        if (PIND & (1 << SOFT_RX))
            data |= (1 << i);
        _delay_us(104);
    }
    _delay_us(104); // Stop bit
    return data;
}

// Function to display Bluetooth command on LCD
void display_bluetooth_command(char cmd) {
    lcd_clear();
    char buffer[17];

    // Display the raw command
    sprintf(buffer, "BT CMD: %c", cmd);
    lcd_print(buffer);

    // Display command meaning on second line
    lcd_set_cursor(0x40);
    switch(cmd) {
        case 'A': lcd_print("Gnd LED Red"); break;
        case 'B': lcd_print("Gnd LED Green"); break;
        case 'C': lcd_print("Gnd LED Blue"); break;
        case 'D': lcd_print("Gnd Fan Low"); break;
        case 'E': lcd_print("Gnd Fan High"); break;
        case 'F': lcd_print("Gnd Fan Off"); break;
        case 'G': lcd_print("Elevator Up"); break;
        case 'H': lcd_print("Elevator Down"); break;
        case 'I': lcd_print("Read Sensor"); break;
        case 'J': lcd_print("1st LED Red"); break;
        case 'K': lcd_print("1st Fan High"); break;
        case 'L': lcd_print("1st Fan Low"); break;
        case 'M': lcd_print("1st Fan Off"); break;
        case 'N': lcd_print("1st LED Green"); break;
        case 'O': lcd_print("1st LED Blue"); break;
        default: lcd_print("Unknown CMD"); break;
    }

    _delay_ms(2000); // Display for 2 seconds
}

// Unified Send Command function with error handling
void send_command(char slave_id, char component, char value) {
    // Input validation
    if(slave_id == 0 || component == 0) {
        lcd_clear();
        lcd_print("Invalid Command!");
        buzzer_beep(500);
        _delay_ms(1000);
        return;
    }

    // Display command being sent
    lcd_clear();
    char buffer[17];
    sprintf(buffer, "Sending: %c%c%c", slave_id, component, value);
    lcd_print(buffer);
    _delay_ms(1000);

    // Send via hardware UART
    USART_Transmit(slave_id);
    USART_Transmit(component);
    USART_Transmit(value);

    // Send via software UART
    uart_tx_char(slave_id);
    uart_tx_char(component);
    uart_tx_char(value);

    // Audible feedback
    PORTB |= (1 << BUZZER_PIN);
    _delay_ms(20);
    PORTB &= ~(1 << BUZZER_PIN);
    _delay_ms(50);
}

// LCD Functions
void lcd_cmd(unsigned char cmd) {
    LCD_PORT = (LCD_PORT & 0xE1) | ((cmd >> 4) << 1);
    PORTD &= ~(1 << RS);
    PORTD |= (1 << E);
    _delay_ms(1);
    PORTD &= ~(1 << E);

    LCD_PORT = (LCD_PORT & 0xE1) | ((cmd & 0x0F) << 1);
    PORTD |= (1 << E);
    _delay_ms(1);
    PORTD &= ~(1 << E);
    _delay_ms(2);
}

void lcd_data(unsigned char data) {
    LCD_PORT = (LCD_PORT & 0xE1) | ((data >> 4) << 1);
    PORTD |= (1 << RS);
    PORTD |= (1 << E);
    _delay_ms(1);
    PORTD &= ~(1 << E);

    LCD_PORT = (LCD_PORT & 0xE1) | ((data & 0x0F) << 1);
    PORTD |= (1 << E);
    _delay_ms(1);
    PORTD &= ~(1 << E);
    _delay_ms(2);
}

void lcd_init() {
    DDRB |= (1 << D4) | (1 << D5) | (1 << D6) | (1 << D7);
    DDRD |= (1 << RS) | (1 << E);

    _delay_ms(20);
    lcd_cmd(0x02); // Return home
    lcd_cmd(0x28); // 4-bit mode, 2-line, 5x8 font
    lcd_cmd(0x0C); // Display on, cursor off
    lcd_cmd(0x06); // Increment mode
    lcd_cmd(0x01); // Clear display
    _delay_ms(2);
}

void lcd_print(char *str) {
    while (*str) {
        lcd_data(*str++);
    }
}

void lcd_clear() {
    lcd_cmd(0x01);
    _delay_ms(2);
}

void lcd_set_cursor(unsigned char pos) {
    lcd_cmd(0x80 | pos);
}

// Buzzer control with validation
void buzzer_beep(uint16_t duration_ms) {
    if(duration_ms > 2000) duration_ms = 2000; // Limit max duration
    PORTB |= (1 << BUZZER_PIN);
    _delay_ms(duration_ms);
    PORTB &= ~(1 << BUZZER_PIN);
}

// Keypad functions with debounce improvement
char keypad_getkey() {
    static unsigned long last_key_time = 0;
    unsigned char row, col;
    char keys[4][4] = {
        {'1', '2', '3', 'A'},
        {'4', '5', '6', 'B'},
        {'7', '8', '9', 'C'},
        {'*', '0', '#', 'D'}
    };

    // Simple debounce using delay
    if(last_key_time > 0 && (_delay_ms_counter - last_key_time) < 100) {
        return 0;
    }

    for (row = 0; row < 4; row++) {
        KEYPAD_PORT = ~(1 << row);
        _delay_us(5);
        for (col = 0; col < 4; col++) {
            if (!(KEYPAD_PIN & (1 << (col + 4)))) {
                // Wait for key release
                while (!(KEYPAD_PIN & (1 << (col + 4))));
                buzzer_beep(50);
                last_key_time = _delay_ms_counter;
                return keys[row][col];
            }
        }
    }
    return 0;
}

// Get user input with timeout
uint8_t get_input(char *buffer, uint8_t length) {
    char key;
    uint8_t i = 0;
    unsigned long start_time = _delay_ms_counter;

    while (i < length) {
        if((_delay_ms_counter - start_time) > 30000) { // 30s timeout
            buffer[0] = '\0';
            return 0; // Timeout occurred
        }

        key = keypad_getkey();
        if(key) {
            buffer[i++] = key;
            lcd_data('*'); // Show asterisk for password
            _delay_ms(100);
        }
    }
    buffer[length] = '\0';
    return 1; // Success
}

// Menu display functions
void show_main_menu() {
    lcd_clear();
    lcd_print("1:Ground Floor");
    lcd_set_cursor(0x40);
    lcd_print("2:First Floor");
}

void show_ground_floor_menu() {
    lcd_clear();
    lcd_print("1:Fan 2:LED");
    lcd_set_cursor(0x40);
    lcd_print("3:Elev *:Sensors");
}

void show_fan_menu(uint8_t floor) {
    lcd_clear();
    lcd_print(floor ? "1st Floor Fan:" : "Gnd Floor Fan:");
    lcd_set_cursor(0x40);
    lcd_print("0:Off 1:Low 2:High");
}

void show_led_menu(uint8_t floor) {
    lcd_clear();
    lcd_print(floor ? "1st Floor LED:" : "Gnd Floor LED:");
    lcd_set_cursor(0x40);
    lcd_print("1:R 2:G 3:B 4:W");
}

void show_elevator_menu() {
    lcd_clear();
    lcd_print("Elevator:");
    lcd_set_cursor(0x40);
    lcd_print("1:Up 2:Down");
}

// Handle unknown commands consistently
void handle_unknown_command() {
    lcd_clear();
    lcd_print("Unknown Command!");
    buzzer_beep(300);
    _delay_ms(1000);

    // Return to current menu
    switch(current_menu) {
        case 0: show_main_menu(); break;
        case 1: show_ground_floor_menu(); break;
        case 2: show_fan_menu(current_floor_menu); break;
        case 3: show_led_menu(current_floor_menu); break;
        case 4: show_elevator_menu(); break;
    }
}

// Improved sensor data display with error handling
void show_sensor_data() {
    lcd_clear();
    lcd_print("Reading Sensor...");

    send_command('S', 'R', '1');
    _delay_ms(100);

    uint8_t temp = USART_Receive();
    uint8_t hum = USART_Receive();

    if(temp == 0xFF || hum == 0xFF) {
        lcd_clear();
        lcd_print("Sensor Error!");
        buzzer_beep(500);
    } else {
        char buffer[16];
        sprintf(buffer, "Temp:%dC Hum:%d%%", temp, hum);
        lcd_clear();
        lcd_print(buffer);
        buzzer_beep(200);
    }
    _delay_ms(3000);
    show_ground_floor_menu();
}

// Floor change function with improved feedback
void change_floor(uint8_t new_floor) {
    if(new_floor == current_floor) return;

    // Turn off current floor devices
    if(current_floor == GROUND_FLOOR) {
        send_command('G', 'F', '0'); // Fan off
        send_command('G', 'L', 'X'); // Lights off
    } else {
        send_command('F', 'F', '0');
        send_command('F', 'L', 'X');
    }

    // Start moving
    send_command('E', (new_floor == GROUND_FLOOR) ? 'D' : 'U', '1');
    lcd_clear();
    lcd_print("Moving...");
    buzzer_beep(500);

    // Simulate movement time (8 seconds)
    for(uint8_t i = 0; i < 8; i++) {
        _delay_ms(1000);
        lcd_set_cursor(0x40);
        lcd_print(".");
    }

    current_floor = new_floor;
    lcd_clear();
    lcd_print(current_floor ? "First Floor" : "Ground Floor");
    buzzer_beep(200);
    _delay_ms(1000);

    // Activate new floor indicator
    send_command(new_floor ? 'F' : 'G', 'I', '1');
    show_ground_floor_menu();
}

// Password change function with improved flow
void handle_password_change() {
    char temp_password[5];
    char confirm_password[5];

    // Get current password
    lcd_clear();
    lcd_print("Enter Current PW:");
    lcd_set_cursor(0x40);
    if(!get_input(temp_password, 4)) {
        lcd_clear();
        lcd_print("Timeout!");
        buzzer_beep(500);
        _delay_ms(2000);
        return;
    }

    // Verify current password
    char stored_password[5];
    eeprom_read_block(stored_password, saved_password, 5);
    if(strncmp(temp_password, stored_password, 4) != 0) {
        lcd_clear();
        lcd_print("Wrong Password!");
        buzzer_beep(500);
        _delay_ms(2000);
        return;
    }

    // Get new password
    lcd_clear();
    lcd_print("Enter New PW:");
    lcd_set_cursor(0x40);
    if(!get_input(temp_password, 4)) {
        lcd_clear();
        lcd_print("Timeout!");
        buzzer_beep(500);
        _delay_ms(2000);
        return;
    }

    // Confirm new password
    lcd_clear();
    lcd_print("Confirm New PW:");
    lcd_set_cursor(0x40);
    if(!get_input(confirm_password, 4)) {
        lcd_clear();
        lcd_print("Timeout!");
        buzzer_beep(500);
        _delay_ms(2000);
        return;
    }

    // Verify match
    if(strncmp(temp_password, confirm_password, 4) != 0) {
        lcd_clear();
        lcd_print("Not Matching!");
        buzzer_beep(500);
        _delay_ms(2000);
        return;
    }

    // Update password
    eeprom_update_block(temp_password, saved_password, 5);
    lcd_clear();
    lcd_print("Password Changed!");
    buzzer_beep(200);
    _delay_ms(2000);
}

// Execute commands from mobile with improved validation
void execute_uart_command(char cmd) {
    // First display the command on LCD
    display_bluetooth_command(cmd);

    // Then execute the command
    switch(cmd) {
        case 'A': send_command('G', 'L', 'R'); break; // Ground Red
        case 'B': send_command('G', 'L', 'G'); break; // Ground Green
        case 'C': send_command('G', 'L', 'B'); break; // Ground Blue
        case 'D': send_command('G', 'F', '1'); break; // Ground Fan Low
        case 'E': send_command('G', 'F', '2'); break; // Ground Fan High
        case 'F': send_command('G', 'F', '0'); break; // Ground Fan Off
        case 'G': send_command('E', 'U', '1'); break; // Elevator Up
        case 'H': send_command('E', 'D', '1'); break; // Elevator Down
        case 'I': send_command('S', 'R', '1'); break; // Sensor Read
        case 'J': send_command('F', 'L', 'R'); break; // First Red
        case 'N': send_command('F', 'L', 'G'); break; // First Green
        case 'O': send_command('F', 'L', 'B'); break; // First Blue
        case 'K': send_command('F', 'F', '2'); break; // First Fan High
        case 'L': send_command('F', 'F', '1'); break; // First Fan Low
        case 'M': send_command('F', 'F', '0'); break; // First Fan Off
        default:
            handle_unknown_command();
            break;
    }
}

// Main function with improved structure
int main(void) {
    // Initialize ports
    DDRB |= (1 << D4) | (1 << D5) | (1 << D6) | (1 << D7) | (1 << BUZZER_PIN);
    DDRD |= (1 << RS) | (1 << E) | (1 << SOFT_TX);
    DDRD &= ~(1 << SOFT_RX);
    PORTD |= (1 << SOFT_RX); // Pull-up
    KEYPAD_DDR = 0x0F;
    KEYPAD_PORT = 0xF0;

    // Initialize components
    lcd_init();
    PWM_init();
    servo_set_angle(90);
    USART_Init(MYUBRR);
    initialize_eeprom();

    char input[5] = {0};
    char password[5] = {0};
    uint8_t attempts = 0;
    char key = 0;

    // Password entry loop with timeout
    while (1) {
        lcd_clear();
        lcd_print("Enter Password:");
        lcd_set_cursor(0x40);

        if(!get_input(input, 4)) {
            lcd_clear();
            lcd_print("Timeout!");
            buzzer_beep(500);
            _delay_ms(2000);
            continue;
        }

        eeprom_read_block((void*)password, (const void*)saved_password, 5);
        if (strncmp(input, password, 4) == 0) {
            eeprom_update_byte(&attempt_count, 0);
            lcd_clear();
            lcd_print("Welcome!");
            buzzer_beep(100);
            servo_set_angle(180);  // Move to 180°
            _delay_ms(3000);


            change_floor(GROUND_FLOOR);
            break;
        } else {
            attempts++;
            eeprom_update_byte(&attempt_count, attempts);
            lcd_clear();
            if (attempts >= 3) {
                lcd_print("LOCKED! Try Later");
                buzzer_beep(3000);
                _delay_ms(5000);
                attempts = 0;
            } else {
                lcd_print("Wrong! Try Again");
                buzzer_beep(500);
                _delay_ms(2000);
            }
        }
    }

    // Main operation loop
    current_menu = 0;
    current_floor_menu = GROUND_FLOOR;
    show_main_menu();

    while(1) {
        // Check for mobile commands
        if(!(PIND & (1 << SOFT_RX))) {
            char mobile_cmd = uart_rx_char();
            if(mobile_cmd != 0xFF) { // Only process if not timeout
                execute_uart_command(mobile_cmd);

                // Return to current menu after command
                switch(current_menu) {
                    case 0: show_main_menu(); break;
                    case 1: show_ground_floor_menu(); break;
                    case 2: show_fan_menu(current_floor_menu); break;
                    case 3: show_led_menu(current_floor_menu); break;
                    case 4: show_elevator_menu(); break;
                }
            }
            _delay_ms(100);
        }

        // Check for keypad input
        key = keypad_getkey();
        if(!key) continue;

        if(key == '*') {
            show_sensor_data();
            continue;
        }

        switch(current_menu) {
            case 0: // Main menu
                if(key == '1') {
                    change_floor(GROUND_FLOOR);
                    current_floor_menu = GROUND_FLOOR;
                    current_menu = 1;
                } else if(key == '2') {
                    change_floor(FIRST_FLOOR);
                    current_floor_menu = FIRST_FLOOR;
                    current_menu = 1;
                } else if(key == 'A') {
                    handle_password_change();
                    show_main_menu();
                } else {
                    handle_unknown_command();
                }
                break;

            case 1: // Floor menu
                if(key == '1') {
                    current_menu = 2;
                    show_fan_menu(current_floor_menu);
                } else if(key == '2') {
                    current_menu = 3;
                    show_led_menu(current_floor_menu);
                } else if(key == '3') {
                    current_menu = 4;
                    show_elevator_menu();
                } else if(key == '#') {
                    current_menu = 0;
                    show_main_menu();
                } else {
                    handle_unknown_command();
                }
                break;

            case 2: // Fan control
                if(key >= '0' && key <= '2') {
                    send_command(current_floor_menu ? 'F' : 'G', 'F', key);
                    lcd_clear();
                    lcd_print("Fan Speed Set");
                    buzzer_beep(200);
                    _delay_ms(1000);
                    show_fan_menu(current_floor_menu);
                } else if(key == '#') {
                    current_menu = 1;
                    show_ground_floor_menu();
                } else {
                    handle_unknown_command();
                }
                break;

            case 3: // LED control
                if(key >= '1' && key <= '4') {
                    char color;
                    switch(key) {
                        case '1': color = 'R'; break;
                        case '2': color = 'G'; break;
                        case '3': color = 'B'; break;
                        case '4': color = 'W'; break;
                    }
                    send_command(current_floor_menu ? 'F' : 'G', 'L', color);
                    lcd_clear();
                    lcd_print("Color Set");
                    buzzer_beep(200);
                    _delay_ms(1000);
                    show_led_menu(current_floor_menu);
                } else if(key == '#') {
                    current_menu = 1;
                    show_ground_floor_menu();
                } else {
                    handle_unknown_command();
                }
                break;

            case 4: // Elevator control
                if(key == '1') {
                    change_floor(FIRST_FLOOR);
                    current_floor_menu = FIRST_FLOOR;
                    current_menu = 1;
                } else if(key == '2') {
                    change_floor(GROUND_FLOOR);
                    current_floor_menu = GROUND_FLOOR;
                    current_menu = 1;
                } else if(key == '#') {
                    current_menu = 1;
                    show_ground_floor_menu();
                } else {
                    handle_unknown_command();
                }
                break;
        }
    }
}
