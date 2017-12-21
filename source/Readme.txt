Instructions for the new custom soft key interface :

Create a text file with your player's name and the extension .csk
(see default.csk).

If no .csk is found, the game will use the default soft key setup.

Each key starts with a '[key name]' (the key name is for you own reference
and is not used by the game).

Attribute format :
attribute=value
spaces are tolerated (ie '   attribute   =  value  ' is the same as
                      'attribute=value')

Possible attributes :
x - the x position (from 0 to 255, negative value will calculate the width of
    the item and offset from the right of the screen, the special value
    16384 will center the item horizontally)
y - the y position (from 0 to 383 (0 - 191 is top screen, 192-383 is bottom
    screen), negative value will calculate the height of the item and offset
    from the bottom of the screen)
w - width of item, if omitted will be calculated were possible
h - height of item, if omitted will be calculated were possible
color - color of the item, in a rgb triplet from 0 to 255
        the actual color maybe not be the specified color, based on the palette
        if no color is specified, the default color is the green color used for
        the ingame text                
type - type of item, valid options are :
       text_big - writes a user defined string with the big game font
       text_med_grey - writes a user defined string with the medium grey game
                       font
       text_med_gold - writes a user defined string with the medium gold game
                       font
       text_med_blue - writes a user defined string with the medium blue game
                       font
       text_small - writes a user defined string with the small
                           game font with a user defined color
       image - draws the user specified image
       automap - draws the automap using the user specified coordinates,
                 requires width and height to be specified
       score - writes the game score
       homing_warning - writes the homing missile warning when active
       enery - writes the user's energe
       shield - writes the user's shield
       lives - writes the user's lives
       primary_weapon - writes the user's currently selected primary weapon
       secondary_weapon - writes the user's currently selected secondary weapon
       keys - draws the user's available keys
       cloak - writes the user's cloaking status when cloaked
       invuln - writes the user's invulnerability status when invulnerable
       time - writes the current game's time
       framerate - writes the framerate
image - image to use when item's type is image
text - text to use when item's type is text_*
key - key to send when the item is touched using the touchscreen. The first
      none space character to be read is used. Use only lower case characters
      (ie , and . instead of < and >)
border - 0 - do not draw border
         1 - to draw border using the specified color

See example.csk for an example adding 2 soft key to enable rolling.
