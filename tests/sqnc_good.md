```sqnc
---
spec-version: "sqnc-1"
title: "Test Flow"
---
LOAD SKILL "pitching"
STEP 1: GATHER
1. ASK USER "Which folder?"
2. SAVE answer INTO VARIABLE `folder`
3. EXECUTE tool `fs_list` with payload { "path": `folder` }
4. SAVE result INTO VARIABLE `files`
STEP 2: WORK
FOR EACH `f` IN `files` DO
    IF `f.name` IS EQUAL TO "notes.txt" THEN
        INVOKE SKILL "pitching" USING context `f`
    ELSE IF `f` IS EMPTY THEN
        Skip it.
    ELSE
        RETRY UP TO 2 TIMES DO
            EXECUTE tool `fs_read` with parameters:
               - path: `f.name`
        END RETRY
    END IF
END FOR
RETURN "Read " + `files.length` + " files"
```
