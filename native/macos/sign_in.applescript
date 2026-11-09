use framework "Foundation"
use scripting additions

on run argv
    set promptKind to item 1 of argv
    if promptKind is "credentials" then
        set accountName to text returned of (display dialog "Apple Account email for the experimental native wrapper:" default answer "" with title "Wrapper native sign-in" buttons {"Cancel", "Continue"} default button "Continue" cancel button "Cancel")
        set accountPassword to text returned of (display dialog "Apple Account password for the experimental native wrapper:" default answer "" with hidden answer with title "Wrapper native sign-in" buttons {"Cancel", "Sign In"} default button "Sign In" cancel button "Cancel")
        return accountName & linefeed & accountPassword
    else if promptKind is "code" then
        return text returned of (display dialog "Enter the current six-digit Apple verification code:" default answer "" with hidden answer with title "Wrapper verification code" buttons {"Cancel", "Continue"} default button "Continue" cancel button "Cancel")
    else if promptKind is "dialog" then
        set inputData to current application's NSFileHandle's fileHandleWithStandardInput()'s readDataToEndOfFile()
        set payload to current application's NSPropertyListSerialization's propertyListWithData:inputData options:0 format:(missing value) |error|:(missing value)
        set dialogTitle to (payload's objectForKey:"title") as text
        set dialogMessage to (payload's objectForKey:"message") as text
        set choices to (payload's objectForKey:"buttons") as list
        if (count of argv) > 1 then
            if item 2 of argv is "parse-only" then return (count of choices) as text
        end if
        set selection to choose from list choices with title "Wrapper sign-in message" with prompt (dialogTitle & return & dialogMessage) OK button name "Continue" cancel button name "Cancel" empty selection allowed false multiple selections allowed false
        if selection is false then return "0"
        repeat with i from 1 to count of choices
            if item i of choices is item 1 of selection then return i as text
        end repeat
        return "0"
    else
        error "Unknown native wrapper prompt"
    end if
end run
