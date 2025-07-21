#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>

// Mocks pour les classes Arduino
namespace Mock {
    class File {
    public:
        bool open(const char* path, const char* mode) {
            std::cout << "[Mock] Ouvrir fichier: " << path << " en mode " << mode << std::endl;
            return true;
        }
        
        void close() {
            std::cout << "[Mock] Fermer fichier" << std::endl;
        }
        
        bool exists(const char* path) {
            std::cout << "[Mock] Vérifier existence: " << path << std::endl;
            return true;
        }
        
        void println(const char* text) {
            std::cout << "[Mock] Écrire: " << text << std::endl;
        }
        
        std::string readString() {
            return "Test réussi!";
        }
    };

    class WiFi {
    public:
        static std::string softAPSSID() {
            return "Jcorp_Nomad";
        }
        
        static std::string softAPPassword() {
            return "password";
        }
        
        static std::string softAPIP() {
            return "192.168.4.1";
        }
    };

    class Serial {
    public:
        static void begin(int baud) {
            std::cout << "[Mock] Initialiser Serial à " << baud << " bauds" << std::endl;
        }
        
        static void println(const std::string& text) {
            std::cout << text << std::endl;
        }
        
        static void println(const char* text) {
            std::cout << text << std::endl;
        }
    };

    class lv_label {
    public:
        static void set_text(const char* text) {
            std::cout << "[Mock] Afficher sur écran: " << text << std::endl;
        }
    };

    static lv_label ui_uilabel;
    static int ui_uiuserlabel;
}

using namespace Mock;

// Simulation de la carte SD
class SD : public File {
public:
    bool exists(const char* path) {
        std::cout << "[SD Mock] Vérifier existence: " << path << std::endl;
        return true;
    }
    
    File open(const char* path, const char* mode) {
        std::cout << "[SD Mock] Ouvrir fichier: " << path << " en mode " << mode << std::endl;
        return File();
    }
};

SD SD;

// Simulation du test système
void runSystemTest() {
    std::cout << "\n=== Début du test système ===" << std::endl;
    
    // Test de l'écran
    std::cout << "Test de l'écran..." << std::endl;
    lv_label::set_text("Test en cours...");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    // Test de la carte SD
    std::cout << "Test de la carte SD..." << std::endl;
    if (!SD.exists("test.txt")) {
        std::cout << "Création du fichier test.txt" << std::endl;
        File testFile = SD.open("test.txt", "w");
        if (testFile.exists("test.txt")) {
            testFile.println("Test réussi!");
            testFile.close();
            std::cout << "Fichier créé avec succès" << std::endl;
        } else {
            std::cout << "ERREUR: Impossible de créer le fichier test.txt" << std::endl;
        }
    }
    
    if (SD.exists("test.txt")) {
        std::cout << "Test de lecture du fichier..." << std::endl;
        File readFile = SD.open("test.txt", "r");
        if (readFile.exists("test.txt")) {
            std::string content = readFile.readString();
            std::cout << "Contenu du fichier: " << content << std::endl;
            readFile.close();
            std::cout << "Test de lecture réussi" << std::endl;
        } else {
            std::cout << "ERREUR: Impossible d'ouvrir le fichier test.txt" << std::endl;
        }
    }
    
    // Test de la connexion WiFi
    std::cout << "Test de la connexion WiFi..." << std::endl;
    std::cout << "SSID: " << WiFi::softAPSSID() << std::endl;
    std::cout << "Mot de passe: " << WiFi::softAPPassword() << std::endl;
    std::cout << "IP: " << WiFi::softAPIP() << std::endl;
    
    std::cout << "=== Fin du test système ===" << std::endl;
}

int main() {
    Serial::begin(115200);
    runSystemTest();
    return 0;
}
