# Third-Party Notices

## Adobe Digital Negative (DNG)

Specification: <https://helpx.adobe.com/camera-raw/digital-negative.html>

This product includes DNG technology under license by Adobe.

## RapidRAW tone pipeline

Source: RapidRAW commit
`d6d8daa999f81198fb49e99b7e8ff43b47a6ffcd`,
`src-tauri/src/shaders/shader.wgsl`, `src-tauri/src/shaders/blur.wgsl`, and
`src-tauri/src/image_processing.rs`,
<https://github.com/CyberTimon/RapidRAW>.

Copyright (c) Timon Käch and RapidRAW contributors. Used under the GNU Affero
General Public License, version 3. Ravo's C++20/QRhi adaptation retains the
Basic RAW display curve, global tone functions, 3.5-pixel tonal blur, and
declared UI normalization while replacing the
Rust/Tauri/wgpu application, worker, texture, and state owners with Ravo's
versioned Recipe, Engine CPU-gold, QRhi preview, cancellation, bounded memory,
and structured-error contracts. Ravo distributes the combined product under
the GNU Affero General Public License, version 3.

## RawTherapee RCD demosaic

Source: <https://github.com/Beep6581/RawTherapee/blob/498f62378/rtengine/rcd_demosaic.cc>

Copyright (c) 2017-2020 Luis Sanz Rodriguez and Ingo Weyrich.

RCD 2.3 and its tiled implementation were adapted under the GNU General Public
License, version 3 or later. Ravo's modified C++20 owner removes RawTherapee
application state, UI/progress callbacks, OpenMP ownership, and the implicit
IGV fallback; it adds bounded task-local storage, cancellation, explicit
errors, and source ownership.

## LibRaw PPG demosaic

Source: <https://github.com/Beep6581/RawTherapee/blob/498f62378/rtengine/libraw/src/demosaic/misc_demosaic.cpp>

Patterned Pixel Grouping interpolation by Alain Desbiolles. Copyright
2019-2025 LibRaw LLC; the source also identifies Dave Coffin's dcraw decoder
copyright 1997-2018. Used under the GNU Lesser General Public License version
2.1 option offered by LibRaw. Ravo's modified floating-point owner removes
LibRaw storage/callback/OpenMP ownership and integer clipping, and adds bounded
cancellation and structured failures.

## Markesteijn X-Trans demosaic and RAW denoise

Sources:

- Frozen darktable sources at repository commit
  `f7ea869a2bd3daafd04186c49f72861b2a574102`:
  `darktable 0.9 src/iop/demosaicing/xtrans.c` and `darktable 0.9 src/iop/rawdenoise.c`.
- RawTherapee commit `498f623784e33fd9a7077fcd8937fe0734033366`:
  <https://github.com/Beep6581/RawTherapee/blob/498f62378/rtengine/xtrans_demosaic.cc>.

Frank Markesteijn's X-Trans algorithm was adapted to RawTherapee by Ingo
Weyrich. The referenced files are used under the GNU General Public License,
version 3 or later. Ravo's modified C++20 owner removes application globals,
UI/progress callbacks, OpenMP scheduling and unsafe multidimensional pointer
arithmetic. It adds crop-phased owned 6×6 CFA metadata, same-CFA preview
reduction, task-local bounded tiles, mirrored borders, cancellation, memory
preflight, finite checks and structured sensor/mode failures. The separate
X-Trans RAW-denoise port retains the frozen RGB nearest-neighbour and
square-root five-band wavelet math while making cancellation publication
atomic.

## ART / darktable perspective correction

Source: <https://github.com/artraweditor/ART/blob/6f511409afe28b2096c38483a6dfa3afcf167f5b/rtengine/perspectivecorrection.cc>

Copyright (c) 2019 Alberto Griggio. The ART source credits darktable
`src/iop/ashift.c`, copyright (c) 2016 Ulrich Pegelow, and the ShiftN work of
Marcus Hebel. Used under the GNU General Public License, version 3 or later.
Ravo adapts the ShiftN-style homography composition into an independent C++20
owner and replaces application state, UI callbacks and OpenMP scheduling with
checked bounds, deterministic safe crop, task-local cancellation, explicit
interpolation and structured failures.

## darktable / OpenColorIO 3D-LUT interpolation research

Sources reviewed:

- Frozen darktable source at repository commit
  `f7ea869a2bd3daafd04186c49f72861b2a574102`,
  `darktable 0.9 src/iop/lut3d.c` and `darktable 0.9 data/data/kernels/lut3d.cl`.
- ART commit `6f511409afe28b2096c38483a6dfa3afcf167f5b`,
  `rtengine/LUT3D.cc`.

The darktable files are copyright (c) 2019–2026 darktable developers and are
available under the GNU General Public License, version 3 or later. Their
trilinear routine credits Eskil Steenberg's BSD-licensed `HaldCLUT_correct.c`;
their tetrahedral routine, and ART's independently reviewed routine, credit the
OpenColorIO project under BSD-3-Clause.

Ravo's C++20 owner independently implements a strict bounded `.cube` parser,
red-fastest indexing, trilinear/tetrahedral interpolation, declared colour
transforms, full-content fingerprinting, immutable LRU lifecycle,
cancellation, and structured failures. It does not copy the application/UI,
OpenCL, Hald image, pyramid, OCIO/CTL subprocess, or global cache ownership.

## vkdt camera-noise fitting research

Sources reviewed at vkdt commit `b95b3a0a`:

- `src/pipe/modules/rawhist/main.c` and its histogram shaders;
- `src/pipe/modules/nprof/main.c`;
- `doc/howto/noise-profiling/readme.md`.

vkdt is copyright its contributors and distributed under the GNU General
Public License, version 3 or later. Ravo retains the documented physical model
`variance = Gaussian + Poisson × signal`, but independently implements a
bounded C++20 fitter and versioned artifact. It does not copy vkdt's Vulkan
graph, shader histogram, global module state, implicit configuration install,
or random fallback coefficients. Ravo adds strict units/schema, deterministic
robust rejection, non-negative fitting, cancellation, SHA-256 verification,
atomic no-replace publication and structured failures.

## ART Texture Boost

Source: <https://github.com/artraweditor/ART/blob/6f511409afe28b2096c38483a6dfa3afcf167f5b/rtengine/iptextureboost.cc>

Copyright (c) 2018 Alberto Griggio. Used under the GNU General Public License,
version 3 or later.

Ravo adapts the two-band strength shaping and guided-luminance decomposition
into an independent C++20 operation. It replaces ART image/application state,
mask and resampling ownership, SIMD/OpenMP scheduling and 16-bit scaling with
explicit linear-Rec.709 input, canonical scale, bounded task-local memory,
cancellation, structured failures, hue-preserving unbounded RGB application
and atomic publication. Local Laplacian was evaluated only in a separate
test-owned decision prototype and is not shipped as an operation.

## Filmulator physical-development research

Sources reviewed at Filmulator commit
`57fbaec57555432d86d3aa632990cd8fa09114ad`:

- <https://github.com/CarVac/filmulator-gui/blob/57fbaec57555432d86d3aa632990cd8fa09114ad/filmulator-gui/core/filmulate.cpp>;
- `core/develop.cpp`, `core/diffuse.cpp`, `core/layerMix.cpp`,
  `core/agitate.cpp`, and `core/exposure.cpp` at the same commit.

Copyright (c) 2013 Omer Mano and Carlo Vaccari. The reviewed files are
available under the GNU General Public License, version 3 or later.

Ravo contains only an isolated test-owned decision prototype of activation,
reaction, diffusion, reservoir exchange and agitation. It uses standard
containers and Ravo inputs and carries no Filmulator matrix, pipeline,
database, dlib, OpenMP or UI owner. Measurements rejected production
integration, so there is no Filmulator recipe schema or runtime dependency.

## Little CMS 2.19.1

Source: <https://github.com/mm2/Little-CMS>

MIT License

Copyright (c) 2023 Marti Maria Saguer

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Exiv2 0.28.8

Source: <https://github.com/Exiv2/exiv2>

SPDX-License-Identifier: GPL-2.0-or-later

This program is part of the Exiv2 distribution.
Copyright (C) 2004-2022 Exiv2 authors

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

---------------------------------------------------------------------------
Note:
Individual files contain the following tag instead of the full license text.

  SPDX-License-Identifier: GPL-2.0-or-later

This enables machine processing of license information based on the SPDX
License Identifiers that are here available: http://spdx.org/licenses/

### License text

```text
		    GNU GENERAL PUBLIC LICENSE
		       Version 2, June 1991

 Copyright (C) 1989, 1991 Free Software Foundation, Inc.
     51 Franklin Street, 5th Floor, Boston, MA 02110-1301 USA.
 Everyone is permitted to copy and distribute verbatim copies
 of this license document, but changing it is not allowed.

			    Preamble

  The licenses for most software are designed to take away your
freedom to share and change it.  By contrast, the GNU General Public
License is intended to guarantee your freedom to share and change free
software--to make sure the software is free for all its users.  This
General Public License applies to most of the Free Software
Foundation's software and to any other program whose authors commit to
using it.  (Some other Free Software Foundation software is covered by
the GNU Library General Public License instead.)  You can apply it to
your programs, too.

  When we speak of free software, we are referring to freedom, not
price.  Our General Public Licenses are designed to make sure that you
have the freedom to distribute copies of free software (and charge for
this service if you wish), that you receive source code or can get it
if you want it, that you can change the software or use pieces of it
in new free programs; and that you know you can do these things.

  To protect your rights, we need to make restrictions that forbid
anyone to deny you these rights or to ask you to surrender the rights.
These restrictions translate to certain responsibilities for you if you
distribute copies of the software, or if you modify it.

  For example, if you distribute copies of such a program, whether
gratis or for a fee, you must give the recipients all the rights that
you have.  You must make sure that they, too, receive or can get the
source code.  And you must show them these terms so they know their
rights.

  We protect your rights with two steps: (1) copyright the software, and
(2) offer you this license which gives you legal permission to copy,
distribute and/or modify the software.

  Also, for each author's protection and ours, we want to make certain
that everyone understands that there is no warranty for this free
software.  If the software is modified by someone else and passed on, we
want its recipients to know that what they have is not the original, so
that any problems introduced by others will not reflect on the original
authors' reputations.

  Finally, any free program is threatened constantly by software
patents.  We wish to avoid the danger that redistributors of a free
program will individually obtain patent licenses, in effect making the
program proprietary.  To prevent this, we have made it clear that any
patent must be licensed for everyone's free use or not licensed at all.

  The precise terms and conditions for copying, distribution and
modification follow.

		    GNU GENERAL PUBLIC LICENSE
   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION

  0. This License applies to any program or other work which contains
a notice placed by the copyright holder saying it may be distributed
under the terms of this General Public License.  The "Program", below,
refers to any such program or work, and a "work based on the Program"
means either the Program or any derivative work under copyright law:
that is to say, a work containing the Program or a portion of it,
either verbatim or with modifications and/or translated into another
language.  (Hereinafter, translation is included without limitation in
the term "modification".)  Each licensee is addressed as "you".

Activities other than copying, distribution and modification are not
covered by this License; they are outside its scope.  The act of
running the Program is not restricted, and the output from the Program
is covered only if its contents constitute a work based on the
Program (independent of having been made by running the Program).
Whether that is true depends on what the Program does.

  1. You may copy and distribute verbatim copies of the Program's
source code as you receive it, in any medium, provided that you
conspicuously and appropriately publish on each copy an appropriate
copyright notice and disclaimer of warranty; keep intact all the
notices that refer to this License and to the absence of any warranty;
and give any other recipients of the Program a copy of this License
along with the Program.

You may charge a fee for the physical act of transferring a copy, and
you may at your option offer warranty protection in exchange for a fee.

  2. You may modify your copy or copies of the Program or any portion
of it, thus forming a work based on the Program, and copy and
distribute such modifications or work under the terms of Section 1
above, provided that you also meet all of these conditions:

    a) You must cause the modified files to carry prominent notices
    stating that you changed the files and the date of any change.

    b) You must cause any work that you distribute or publish, that in
    whole or in part contains or is derived from the Program or any
    part thereof, to be licensed as a whole at no charge to all third
    parties under the terms of this License.

    c) If the modified program normally reads commands interactively
    when run, you must cause it, when started running for such
    interactive use in the most ordinary way, to print or display an
    announcement including an appropriate copyright notice and a
    notice that there is no warranty (or else, saying that you provide
    a warranty) and that users may redistribute the program under
    these conditions, and telling the user how to view a copy of this
    License.  (Exception: if the Program itself is interactive but
    does not normally print such an announcement, your work based on
    the Program is not required to print an announcement.)

These requirements apply to the modified work as a whole.  If
identifiable sections of that work are not derived from the Program,
and can be reasonably considered independent and separate works in
themselves, then this License, and its terms, do not apply to those
sections when you distribute them as separate works.  But when you
distribute the same sections as part of a whole which is a work based
on the Program, the distribution of the whole must be on the terms of
this License, whose permissions for other licensees extend to the
entire whole, and thus to each and every part regardless of who wrote it.

Thus, it is not the intent of this section to claim rights or contest
your rights to work written entirely by you; rather, the intent is to
exercise the right to control the distribution of derivative or
collective works based on the Program.

In addition, mere aggregation of another work not based on the Program
with the Program (or with a work based on the Program) on a volume of
a storage or distribution medium does not bring the other work under
the scope of this License.

  3. You may copy and distribute the Program (or a work based on it,
under Section 2) in object code or executable form under the terms of
Sections 1 and 2 above provided that you also do one of the following:

    a) Accompany it with the complete corresponding machine-readable
    source code, which must be distributed under the terms of Sections
    1 and 2 above on a medium customarily used for software interchange; or,

    b) Accompany it with a written offer, valid for at least three
    years, to give any third party, for a charge no more than your
    cost of physically performing source distribution, a complete
    machine-readable copy of the corresponding source code, to be
    distributed under the terms of Sections 1 and 2 above on a medium
    customarily used for software interchange; or,

    c) Accompany it with the information you received as to the offer
    to distribute corresponding source code.  (This alternative is
    allowed only for noncommercial distribution and only if you
    received the program in object code or executable form with such
    an offer, in accord with Subsection b above.)

The source code for a work means the preferred form of the work for
making modifications to it.  For an executable work, complete source
code means all the source code for all modules it contains, plus any
associated interface definition files, plus the scripts used to
control compilation and installation of the executable.  However, as a
special exception, the source code distributed need not include
anything that is normally distributed (in either source or binary
form) with the major components (compiler, kernel, and so on) of the
operating system on which the executable runs, unless that component
itself accompanies the executable.

If distribution of executable or object code is made by offering
access to copy from a designated place, then offering equivalent
access to copy the source code from the same place counts as
distribution of the source code, even though third parties are not
compelled to copy the source along with the object code.

  4. You may not copy, modify, sublicense, or distribute the Program
except as expressly provided under this License.  Any attempt
otherwise to copy, modify, sublicense or distribute the Program is
void, and will automatically terminate your rights under this License.
However, parties who have received copies, or rights, from you under
this License will not have their licenses terminated so long as such
parties remain in full compliance.

  5. You are not required to accept this License, since you have not
signed it.  However, nothing else grants you permission to modify or
distribute the Program or its derivative works.  These actions are
prohibited by law if you do not accept this License.  Therefore, by
modifying or distributing the Program (or any work based on the
Program), you indicate your acceptance of this License to do so, and
all its terms and conditions for copying, distributing or modifying
the Program or works based on it.

  6. Each time you redistribute the Program (or any work based on the
Program), the recipient automatically receives a license from the
original licensor to copy, distribute or modify the Program subject to
these terms and conditions.  You may not impose any further
restrictions on the recipients' exercise of the rights granted herein.
You are not responsible for enforcing compliance by third parties to
this License.

  7. If, as a consequence of a court judgment or allegation of patent
infringement or for any other reason (not limited to patent issues),
conditions are imposed on you (whether by court order, agreement or
otherwise) that contradict the conditions of this License, they do not
excuse you from the conditions of this License.  If you cannot
distribute so as to satisfy simultaneously your obligations under this
License and any other pertinent obligations, then as a consequence you
may not distribute the Program at all.  For example, if a patent
license would not permit royalty-free redistribution of the Program by
all those who receive copies directly or indirectly through you, then
the only way you could satisfy both it and this License would be to
refrain entirely from distribution of the Program.

If any portion of this section is held invalid or unenforceable under
any particular circumstance, the balance of the section is intended to
apply and the section as a whole is intended to apply in other
circumstances.

It is not the purpose of this section to induce you to infringe any
patents or other property right claims or to contest validity of any
such claims; this section has the sole purpose of protecting the
integrity of the free software distribution system, which is
implemented by public license practices.  Many people have made
generous contributions to the wide range of software distributed
through that system in reliance on consistent application of that
system; it is up to the author/donor to decide if he or she is willing
to distribute software through any other system and a licensee cannot
impose that choice.

This section is intended to make thoroughly clear what is believed to
be a consequence of the rest of this License.

  8. If the distribution and/or use of the Program is restricted in
certain countries either by patents or by copyrighted interfaces, the
original copyright holder who places the Program under this License
may add an explicit geographical distribution limitation excluding
those countries, so that distribution is permitted only in or among
countries not thus excluded.  In such case, this License incorporates
the limitation as if written in the body of this License.

  9. The Free Software Foundation may publish revised and/or new versions
of the General Public License from time to time.  Such new versions will
be similar in spirit to the present version, but may differ in detail to
address new problems or concerns.

Each version is given a distinguishing version number.  If the Program
specifies a version number of this License which applies to it and "any
later version", you have the option of following the terms and conditions
either of that version or of any later version published by the Free
Software Foundation.  If the Program does not specify a version number of
this License, you may choose any version ever published by the Free Software
Foundation.

  10. If you wish to incorporate parts of the Program into other free
programs whose distribution conditions are different, write to the author
to ask for permission.  For software which is copyrighted by the Free
Software Foundation, write to the Free Software Foundation; we sometimes
make exceptions for this.  Our decision will be guided by the two goals
of preserving the free status of all derivatives of our free software and
of promoting the sharing and reuse of software generally.

			    NO WARRANTY

  11. BECAUSE THE PROGRAM IS LICENSED FREE OF CHARGE, THERE IS NO WARRANTY
FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW.  EXCEPT WHEN
OTHERWISE STATED IN WRITING THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES
PROVIDE THE PROGRAM "AS IS" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED
OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  THE ENTIRE RISK AS
TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU.  SHOULD THE
PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING,
REPAIR OR CORRECTION.

  12. IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING
WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MAY MODIFY AND/OR
REDISTRIBUTE THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES,
INCLUDING ANY GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING
OUT OF THE USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED
TO LOSS OF DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY
YOU OR THIRD PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER
PROGRAMS), EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE
POSSIBILITY OF SUCH DAMAGES.

		     END OF TERMS AND CONDITIONS

	    How to Apply These Terms to Your New Programs

  If you develop a new program, and you want it to be of the greatest
possible use to the public, the best way to achieve this is to make it
free software which everyone can redistribute and change under these terms.

  To do so, attach the following notices to the program.  It is safest
to attach them to the start of each source file to most effectively
convey the exclusion of warranty; and each file should have at least
the "copyright" line and a pointer to where the full notice is found.

    <one line to give the program's name and a brief idea of what it does.>
    Copyright (C) 19yy  <name of author>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


Also add information on how to contact you by electronic and paper mail.

If the program is interactive, make it output a short notice like this
when it starts in an interactive mode:

    Gnomovision version 69, Copyright (C) 19yy name of author
    Gnomovision comes with ABSOLUTELY NO WARRANTY; for details type `show w'.
    This is free software, and you are welcome to redistribute it
    under certain conditions; type `show c' for details.

The hypothetical commands `show w' and `show c' should show the appropriate
parts of the General Public License.  Of course, the commands you use may
be called something other than `show w' and `show c'; they could even be
mouse-clicks or menu items--whatever suits your program.

You should also get your employer (if you work as a programmer) or your
school, if any, to sign a "copyright disclaimer" for the program, if
necessary.  Here is a sample; alter the names:

  Yoyodyne, Inc., hereby disclaims all copyright interest in the program
  `Gnomovision' (which makes passes at compilers) written by James Hacker.

  <signature of Ty Coon>, 1 April 1989
  Ty Coon, President of Vice

This General Public License does not permit incorporating your program into
proprietary programs.  If your program is a subroutine library, you may
consider it more useful to permit linking proprietary applications with the
library.  If this is what you want to do, use the GNU Library General
Public License instead of this License.
```
