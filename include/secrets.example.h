#pragma once

// ============================================================
//  SECRETS — copier ce fichier vers secrets.h et renseigner
//  vos valeurs avant compilation.
//
//    cp include/secrets.example.h include/secrets.h
//
//  Ce fichier est suivi par git (aucune valeur réelle).
//  secrets.h est ignoré par git (contient les vraies valeurs).
// ============================================================

// --- OTA (Over-The-Air updates) -----------------------------
// Mot de passe exigé lors de chaque upload OTA.
// Doit correspondre à la variable d'environnement OTA_PASSWORD
// utilisée côté client PlatformIO (voir platformio.ini).
#define OTA_PASSWORD  "change_me"
