# Modifications effectuées

## Format de retour standardisé (JSON)

Toutes les commandes retournent maintenant un objet JSON:
```json
{
    "code": 4,
    "message": "...",
    "affectedRows": 0
}
```

| Commande | Format retour |
|---------|-------------|
| `g` (get) | `{"code": 4, "message": "...", "affectedRows": 1}` |
| `s` (set) | `{"code": 4, "message": "Value set", "affectedRows": 1}` |
| `d` (del) | `{"code": 4, "message": "Key deleted", "affectedRows": 1}` |
| `gb` (getby) | `{"code": 4, "message": "...", "affectedRows": n}` |
| `sb` (setby) | `{"code": 4, "message": "Values updated", "affectedRows": n}` |
| `db` (delby) | `{"code": 4, "message": "Keys deleted", "affectedRows": n}` |
| `f` (flush) | `{"code": 4, "message": "Flush successful", "affectedRows": 0}` |
| `h` (help) | `{"code": 4, "message": "...", "affectedRows": 0}` |

## Codes d'erreur utilisés

| Code | Signification |
|------|------------|
| 0 | KEY_TOO_LONG |
| 1 | KEY_NOT_FOUND |
| 2 | VALUE_TOO_LONG |
| 3 | VALUE_EMPTY |
| 4 | SUCCESS |
| 5 | FAIL |
| 7 | PATTERN_EMPTY |
| 8 | INVALID_REGEX |
| 9 | MISSING_KEY |
| 10 | MISSING_VALUE |
| 11 | MISSING_PATTERN |
| 12 | CMD_NOT_FOUND |
| 14 | FLUSH_SUCCESSFUL |
| 15 | JSON_ONLY_ERROR |
| 16 | READ_ONLY_ERROR |
| 17 | NO_COMMAND_GIVEN |
| 18 | INVALID_MODE |