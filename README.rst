
ubackup
*******

Overview
========

ubackup is an incremental backup tool for Unix. Features are

* ubackup can backup remote directories to a local directory.
* ubackup can backup local directories to a remote directory.
* ubackup can backup local directories to another local directory.
* ubackup can use SSH for remote backup.
* One backup is placed into into
  ``year-month-dayThour:minute:second,millisecond`` (datetime in format of
  ISO-8601) directory.
* ubackup uses hard link for incremental backup. If a file is unchanged, ubackup
  makes a hard link to the previous one.
* ubackup backups not only files itself, but also metadata such as permissions
  and owners under ``.meta`` directories.
* ubackup can handle UTF-8 filename.
* ubackup is open source software under the MIT license.

Requirements
============

ubackup works on/with

* FreeBSD 9.1/amd64
* `flange`_

.. _flange: http://github.com/SumiTomohiko/flange

Download
========

tarballs are available at `the author's website`_.

.. _the author's website: http://neko-daisuki.ddo.jp/~SumiTomohiko/repos/

How to install
==============

Requirements
------------

Compiling ubackup needs

* `CMake`_ 2.8.11.2

.. _CMake: http://www.cmake.org/

Edit configure.conf
-------------------

First of all, you must describe settings in configure.conf.
configure.conf.sample is a sample of it::

    $ cp configure.conf.sample configure.conf
    $ vi configure.conf

Configure and compile
---------------------

Now is the time to compile::

    $ ./configure && make && make install

How to use
==========

There are two tools in ubackup -- ubackupme and ubackupyou. ubackupme is one to
backup local directories to a remote machine. ubackupyou is a tool to backup
remote directories into a local directory.

Backup local directories into a remote directory
------------------------------------------------

If you want to backup your local directories to a remote directory, the command
which you must use is::

    ubackupme ssh hostname srcdirs... destdir

``hostname`` must be replaced with name of a remote machine.

All arguments from the fourth one but the last are source directoires. The last
argument is a destination directory in a remote machine.

Backup remote directories into a local directory
------------------------------------------------

To backup remote directories into a local directory, the command is::

    ubackupyou ssh hostname srcdirs... destdir

Backup local directoires into a local directory
-----------------------------------------------

ubackup is also useful to backup local directories into another local directory
in an external storage. You can use both of ubackupme and ubackupyou::

    ubackupme local srcdirs... destdir
    ubackupyou local srcdirs... destdir

Results of these two commands are same.

Structure of a backup directory
===============================

``.meta`` directory
-------------------

Each directory has a ``.meta`` directory, which contains meta data of files in
the directory. A meta data file of ``foo`` is ``.meta/foo.meta``. This file has
mode, uid and gid in each line.

Backup from the root
--------------------

Even if you do::

    $ cd /foo/bar/baz
    $ ubackupme local quux /backup

ubackup backups the quux at ``/backup/timestamp/foo/bar/baz/quux``, not at
``/backup/timestamp/quux``. This means that ubackup does not backup only the
given file, but also the parent directoires of it.

Protocol
========

The line terminator
-------------------

ubackup uses CRLF for the line terminator.

DIR command
-----------

Format: DIR name mode uid gid ctime
Response: OK or NG

FILE command
------------

Format: FILE name mode uid gid mtime ctime
Response: CHANGED, UNCHANGED or NG

A backuper must memorize name for the next BODY command.

BODY command
------------

Format: BODY size
Response: OK or NG

File body follows after a CRLF. A backupee must specify filename with FILE
command previously.

SYMLINK command
---------------

Format: SYMLINK name mode uid gid ctime src
Response: OK or NG

THANK_YOU command
-----------------

Format: THANK_YOU
Response: Nothing (This does not mean "Nothing" command)

A backupee declares the end of connection by this command. A backuper
disconnects immediately without any responses.

The author
==========

The author is `Tomohiko Sumi`_.

.. _Tomohiko Sumi: http://neko-daisuki.ddo.jp/~SumiTomohiko/

.. vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4 filetype=rst
