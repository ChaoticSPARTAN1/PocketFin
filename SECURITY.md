# Security

## Reporting a vulnerability

Please report security problems **privately**, not in a public issue: use GitHub's **Report a
vulnerability** button on the repository's **Security** tab (private vulnerability reporting).
Describe the problem and how to reproduce it. There is no guarantee of a response time.

Examples: a way for data on the link cable or in a library file to crash the app or run code, a tool
that leaks a token or address, or a file that could harm a card.

## Never post publicly

- Wi-Fi network names (SSIDs) or passwords
- Jellyfin server addresses, user names, user or item IDs
- passwords, API keys, access tokens or Quick Connect codes
- `private/jellyfin-connection.txt` or any file from `private/`
- logs, screenshots or photos that show any of the above

If you posted one by mistake, change the password or revoke the token in Jellyfin first, then edit the
post.

## What PocketFin keeps where

- The Pocket never receives your server address or sign-in. Library files on the card hold titles,
  descriptions, pictures and video only.
- The PC tools read your sign-in only from `private/jellyfin-connection.txt` on your PC (Git ignores the
  folder), never print it, and only read from your server.
- The Chromatic side (Jellymatic) keeps its own settings on the Chromatic; see its documentation.
