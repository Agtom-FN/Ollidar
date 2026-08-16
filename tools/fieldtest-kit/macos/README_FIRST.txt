EQUIPMENT TEST - MAC (Apple Silicon: M1 / M2 / M3 / M4)
=======================================================

Takes about five minutes. Nothing gets installed.

NOTE: this kit is built for Apple Silicon Macs. On an older Intel Mac
the test programs will not start - tell us and we will send an Intel
build.  (To check: Apple menu -> About This Mac. It should say "Chip:
Apple M...".)


WHAT YOU HAVE
  1. A small silver/black lidar that SPINS, on a USB cable.
  2. A bigger round lidar, with a NETWORK cable and a 12 volt power lead.
  3. A GPS board with one or two ANTENNAS that screw on, on a USB cable.

You can test them one at a time.


STEP 1  PLUG IN WHAT YOU HAVE
  - Small lidar: plug its USB cable into the Mac.
  - Big round lidar: connect its 12 volt power AND put its network cable
    into the Mac (or into a USB-C-to-Ethernet adapter).
  - GPS: screw the antenna onto the socket marked ANT1 and put the
    antenna OUTSIDE or right against a window. Then plug in its USB.


STEP 2  RIGHT-CLICK THE TEST YOU WANT, THEN CHOOSE "OPEN"
  ==> RIGHT-CLICK (or hold Control and click), then choose  Open .
  ==> Then click  Open  again in the box that appears.

  Do NOT just double-click the first time. macOS blocks files that
  arrived from the internet unless you open them this way once. After
  the first time, a normal double-click works.

  The files:

      TEST_ALL.command         all three, one after the other
                               <-- use this if you are not sure
      TEST_D6.command          only the small spinning lidar
      TEST_MID360.command      only the big round lidar
      TEST_GPS_UM982.command   only the GPS

  A Terminal window opens and tells you what to do.


STEP 3  SEND THE RESULT BACK
  A folder called  LIDAR_TEST_RESULT  appears on your Desktop.
  Send that WHOLE folder back the same way you got this test.


THINGS THAT MIGHT COME UP
  "...cannot be opened because it is from an unidentified developer"
      You double-clicked instead of right-click -> Open. Close the box,
      right-click the file, choose Open, then Open again.

  It asks for your Mac password
      Only in the big-round-lidar test, and only if the Mac does not yet
      have a network address on the lidar's network. Type your normal
      Mac login password (nothing appears on screen as you type) and
      press ENTER. The test prints the one-line command to undo it
      afterwards.

  The GPS says PARTLY WORKED / no satellites
      The antenna cannot see enough sky. Put it outdoors or right on a
      windowsill, wait two minutes, run the GPS test again. Send the
      result folder either way.

  The GPS says SINGLE and not RTK
      That is CORRECT for this test. Centimetre-accurate RTK needs an
      internet correction service that we are not using yet.

  Something else went wrong
      The window explains what to try, in order. If it still fails,
      take a PHOTO of the whole window and send the photo along with the
      Desktop folder.


WHAT THIS KIT CONTAINS
  TEST_*.command   the four things you can double-click
  lib/             the shared script the tests use
  bin/             three small pre-built programs that do the actual
                   recording. They are the whole reason nothing needs
                   installing.

  It only LISTENS to the equipment and saves what it hears. It does not
  install anything, and it does not send anything anywhere - you send
  the folder yourself.
