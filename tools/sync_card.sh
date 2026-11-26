#!/bin/zsh
# Usage: sync_card.sh [VOLUME]
# Copies the reference packs, media samples and mode images from ~/a11-staging and modes/sd to the SD card while it is in the Mac.
# Adds and updates. The only thing it deletes is a pack named in the retired list below. Default card name is /Volumes/CASSIOPEIA.
VOL=${1:-/Volumes/CASSIOPEIA}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
[ -d "$VOL" ] || { echo "No card at $VOL. Put it in the Mac first."; exit 1; }
echo "Free on the card: $(df -h "$VOL" | tail -1 | awk '{print $4}')"
mkdir -p "$VOL/library" "$VOL/media" "$VOL/modes"
for old in cd3wd wikibooks; do [ -d "$VOL/library/$old" ] && rm -rf "$VOL/library/$old" && echo "Removed retired pack $old"; done   # packs taken out of the library
RS=(rsync -rt --modify-window=2 --exclude='._*' --exclude='*.tmp' --exclude='.DS_Store')
"${RS[@]}" "$HERE/modes/sd/library/" "$VOL/library/" || { echo "Copy failed, the card is not complete."; exit 1; }       # Start here, synonyms, Notes
"${RS[@]}" ~/a11-staging/packs/ "$VOL/library/" || { echo "Copy failed, the card is not complete."; exit 1; }             # reference packs
"${RS[@]}" ~/a11-staging/media/ "$VOL/media/" || { echo "Copy failed, the card is not complete."; exit 1; }               # pictures and sound
"${RS[@]}" "$HERE/modes/sd/modes/" "$VOL/modes/" || { echo "Copy failed, the card is not complete."; exit 1; }            # mode images for the loader
dot_clean -m "$VOL" 2>/dev/null
echo "Copy finished. Used on the card now: $(du -sh "$VOL/library" | cut -f1) library, $(du -sh "$VOL/media" | cut -f1) media"
echo "Eject it in Finder, put it back in the Casio."
