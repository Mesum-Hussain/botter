```sqnc
---
spec-version: "sqnc-1"
title: "Interpreter test"
---
STEP 1: GATHER
1. ASK USER "What is your name?"
2. SAVE answer INTO VARIABLE `name`
3. EXECUTE tool `shell_exec` with payload { "command": "echo tool-ran" }
4. SAVE result INTO VARIABLE `out`
5. SET `items` TO ["a", "b", "c"]

STEP 2: WORK
FOR EACH `it` IN `items` DO
    IF `it` IS EQUAL TO "b" THEN
        Mention the letter `it`.
    END IF
END FOR
IF the user seems happy THEN
    SET `mood` TO "happy"
ELSE
    SET `mood` TO "sad"
END IF
RETRY UP TO 2 TIMES DO
    EXECUTE tool `fs_read` with payload { "path": "missing.txt" }
END RETRY
SAVE `items` TO FILE "out/items.json"

STEP 3: DONE
RETURN "Hi " + `name` + ", mood " + `mood` + ", " + `items.length` + " items"
```
