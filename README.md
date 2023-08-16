Project has left GitHub
-----------------------

It is now here: [https://codeberg.org/a-j-wood/continual-sync](https://codeberg.org/a-j-wood/continual-sync)

This project was briefly hosted on GitHub.  GitHub is a proprietary,
trade-secret system that is not Free and Open Source Software (FOSS).

Read about the [Give up GitHub](https://GiveUpGitHub.org) campaign from
[the Software Freedom Conservancy](https://sfconservancy.org) to understand
some of the reasons why GitHub is not a good place to host FOSS projects.

Any use of this project's code by GitHub Copilot, past or present, is done
without permission.  The project owner does not consent to GitHub's use of
this project's code in Copilot.

![Logo of the GiveUpGitHub campaign](https://sfconservancy.org/img/GiveUpGitHub.png)


Introduction
------------

This is the README for "continual-sync", a tool to efficiently keep a
directory synchronised with its mirror.  It uses rsync and the inotify
change notification mechanism to ensure that rsync does as little work as
possible.


Documentation
-------------

Manual pages are included in this distribution.  See "`man continual-sync`",
"`man watchdir`", and "`man continual-sync.conf`".


Compilation
-----------

To compile the package, type "`make`".  Use "`make install`" to install it.


Author
------

This package is copyright (C) 2021 Andrew Wood, and is being distributed
under the terms of the Artistic License 2.0.  For more details, see the
file "COPYING".

You can contact me by using the contact form on my web page at
http://www.ivarch.com/.

The "continual-sync" home page is at:

  http://www.ivarch.com/programs/continual-sync.shtml

The latest version can always be found here.


-----------------------------------------------------------------------------
