# blikk-attest-check

Litet Python-skript som loggar in mot [Blikks publika REST-API](https://publicapidocs.blikk.com/)
och kontrollerar attestflaggorna för **din egen** innevarande månads
tidrapporter — det arbetar alltid mot exakt en Blikk-användare, den som äger
den konfigurerade API-nyckeln. Byggt för att köras som ett steg i ett
automationsflöde (Power Automate/Copilot Studio, Google Apps Script + Cloud
Scheduler, cron, ...) — det skriver en maskinläsbar JSON-summering på stdout
och signalerar resultatet via sin exit-kod, så flödet kan grena på "allt
attesterat" / "det finns tidrapporter som väntar".

**Om "inloggad användare":** Blikks publika API autentiserar en
*applikation* (ett id/secret-par), inte en person — det finns inget
"vem är jag"-anrop att härleda en användare från token med. Skriptet kräver
därför att du anger ditt Blikk-användar-id (`BLIKK_USER_ID`/`--user-id`)
och vägrar köra utan det, istället för att i tysthet falla tillbaka på alla
användare applikationsnyckeln råkar ha läsbehörighet till.

Skriptet är en enda fil (`blikk_attest_check.py`) och använder bara
Python-standardbiblioteket — inget `pip install`, ingen kompilering. Kör med
`python3` på Windows, Linux eller macOS.

## Vad den gör

1. Hämtar en åtkomsttoken via `POST /v1/Auth/Token` (Basic-auth med
   applikations-id/secret).
2. Hämtar tidrapporterna för den konfigurerade användaren för innevarande
   (eller angiven) månad via `GET /v1/Core/TimeReports?filter.userIds=...`,
   paginerat, med automatisk backoff/retry vid `429 Too Many Requests`
   (API:et tillåter max 4 anrop/sekund). Som extra skyddsnät filtrerar
   skriptet även bort eventuella rader som ändå skulle komma tillbaka för
   ett annat användar-id.
3. Härleder attestläget per tidrapport från fälten `sentToAttestDate` /
   `attestedDate`:
   - **notSentToAttest** – inte ens skickad till attest.
   - **sentAwaitingAttest** – skickad, väntar på attestant.
   - **attested** – attesterad.
4. Summerar resultatet och avslutar med en exit-kod som beskriver läget.

## Krav

- Python 3.8 eller senare. Inga tredjepartspaket.

## Konfiguration

Programmet behöver en Blikk API-applikation (id + secret) samt ditt eget
Blikk-användar-id. Applikationen skapas av en administratör i Blikk under
integrations-/API-inställningarna; se till att den har behörigheten
`Timereport_Read`.

| Miljövariabel      | Motsvarande flagga | Beskrivning                                              |
|--------------------|---------------------|-----------------------------------------------------------|
| `BLIKK_APP_ID`      | `--app-id`          | Applikations-id (obligatorisk)                            |
| `BLIKK_APP_SECRET`  | `--app-secret`      | Applikationens secret (obligatorisk)                       |
| `BLIKK_USER_ID`     | `--user-id`         | Ditt Blikk-användar-id (obligatorisk). Endast denna användares tidrapporter hämtas. |
| `BLIKK_BASE_URL`    | `--base-url`        | Override av bas-URL. Standard: `https://publicapi.blikk.com` |

Ditt användar-id hittas t.ex. via `GET /v1/Core/Users` (filtrera på ditt
namn/din e-post) eller i Blikks admin-gränssnitt. Håll `BLIKK_APP_SECRET`
som en hemlighet i flödets/plattformens secret store — lägg den aldrig i
klartext i ett skript eller en commit.

## Användning

```sh
export BLIKK_APP_ID=...
export BLIKK_APP_SECRET=...
export BLIKK_USER_ID=1              # ditt Blikk-användar-id

python3 blikk_attest_check.py                # innevarande månad, JSON på stdout
python3 blikk_attest_check.py --format text  # läsbart format
python3 blikk_attest_check.py --month 2026-08
```

Fullständig flagglista: `python3 blikk_attest_check.py --help`.

### Exit-koder

| Kod | Betydelse                                                           |
|-----|------------------------------------------------------------------------|
| 0   | Anropet lyckades och samtliga tidrapporter i perioden är attesterade   |
| 1   | Anropet lyckades, men minst en tidrapport är inte (ännu) attesterad, eller inga tidrapporter alls hittades för perioden |
| 2   | Fel användning: ogiltiga flaggor eller saknad konfiguration            |
| 3   | Fel mot Blikk-API:et: inloggning eller förfrågan misslyckades          |

Ett automationsflöde kan i regel bara bry sig om `0` (allt klart) kontra
`≠0` (kolla vidare), men skillnaden mellan 1/2/3 gör det enkelt att larma
olika beroende på om det är en riktig attestrestpost eller ett trasigt
autentiseringssteg.

### JSON-utdata

```json
{
  "objectName": "blikkAttestCheck.summary",
  "generatedAt": "2026-09-09T10:00:00Z",
  "month": "2026-09",
  "from": "2026-09-01",
  "to": "2026-09-30",
  "totalReports": 20,
  "totalHours": 160.5,
  "notSentToAttest": 2,
  "sentAwaitingAttest": 3,
  "attested": 15,
  "allAttested": false,
  "users": [
    {
      "userId": 1,
      "userName": "John Doe",
      "totalReports": 20,
      "totalHours": 160.5,
      "notSentToAttest": 2,
      "sentAwaitingAttest": 3,
      "attested": 15,
      "allAttested": false
    }
  ],
  "pendingReports": [
    { "id": 4711, "date": "2026-09-05", "userId": 1, "userName": "John Doe",
      "hours": 8, "status": "notSentToAttest" }
  ]
}
```

`pendingReports` innehåller varje ej attesterad tidrapport, så flödet kan
t.ex. bygga ett Teams-/mejlmeddelande med exakt vilka dagar som saknas.

## Använda i automationsflöden

- **Power Automate (Desktop flow / Copilot Studio)**: kör skriptet med
  åtgärden "Kör DOS-kommando" / "Run application"
  (`python blikk_attest_check.py --format json`) på en maskin med Python
  installerat, fånga stdout i en variabel, tolka den med "Analysera JSON"
  och grena på `%ErrorLevel%` respektive `allAttested`.
- **Google (Apps Script / Cloud Scheduler)**: kör skriptet på en
  Compute Engine-instans/VM som Apps Script kan trigga via HTTP eller SSH,
  eller paketera det i en container och kör som ett schemalagt
  Cloud Run-jobb; låt jobbet skicka `pendingReports` vidare till t.ex.
  Gmail/Chat.
- **Vanlig cron/CI**: kör skriptet som ett steg, kontrollera exit-koden
  direkt i skalet.

## Källa till API-detaljer

Endpoints, filter och fältnamn i den här implementationen är hämtade från
den publika Blikk API-dokumentationen:
<https://publicapidocs.blikk.com/#core-resources-timereports-list>.
`filter.userIds` är dokumenterat som en "integer array"; dokumentationen
visar inte ett konkret exempel på formatet, så anropet i
`fetch_month_timereports()` i `blikk_attest_check.py` skickar det som en
vanlig query-parameter (`filter.userIds=<ditt-id>`), vilket funkar oavsett
om Blikk tolkar en enstaka array-parameter eller en kommaseparerad lista.
Skriptets egen efterfiltrering på `userId` gör att en felaktig tolkning på
Blikks sida ändå aldrig läcker någon annans tidrapporter till utdata.

## Licens

MIT, se [LICENSE](LICENSE).
