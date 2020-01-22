#!/usr/bin/env python3
# coding: utf8
#
# This file is part of Tartan.
# Copyright © 2019 Philip Chimento
#
# Tartan is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Tartan is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Tartan.  If not, see <http://www.gnu.org/licenses/>.

# Take an input file which contains a header of the form:
# /* Template: [template name] */
# followed by a blank line, then one or more sections of the form:
# /*
# [Error message|‘No error’]
# */
# {
#     [Code]
# }
#
# Sections are separated by ‘/*’ on a line of its own. The code must not
# contain C-style comments (‘/* … */’), but can contain C++-style
# ones (‘// …’).
#
# The wrapper script takes each section and wraps the code in a main() function
# with some standard variables and reference count handling provided by the
# named template. It then compiles the code using Clang with Tartan, and checks
# the compiler output against the expected error message. If the expected error
# message is ‘No error’ it asserts there’s no error.

import argparse
import itertools
import os
import re
import shlex
import subprocess
import sys
import tempfile

tests_dir = sys.path[0]
tartan = os.path.join(tests_dir, '..', 'scripts', 'tartan')

parser = argparse.ArgumentParser(description='Tartan test driver.')
parser.add_argument('infile', type=argparse.FileType('r'), metavar='FILE',
                    help='path to input test file')
parser.add_argument('--verbose', '-v', action='store_true',
                    help='print extra log messages')
parser.add_argument('--target-cc', action='store',
                    help='path to clang compiler that plugin targets')
args = parser.parse_args()

# Before starting, work out the compiler’s system include paths.
# Thanks to: http://stackoverflow.com/a/17940271/2931197
result = subprocess.run(['cpp', '-xc++', '-Wp,-v'], input='',
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                        universal_newlines=True)
lines = result.stderr.splitlines()
system_paths = filter(lambda l: l.startswith(' '), lines)
system_paths = map(lambda l: l.replace('(framework directory)', '').strip(),
                   system_paths)
system_includes = []
for path in system_paths:
    system_includes += ['-isystem', path]

output = subprocess.check_output(['pkg-config', '--cflags', 'glib-2.0'],
                                 universal_newlines=True)
glib_includes = shlex.split(output)


class Test:
    ids = itertools.count(0)

    def __init__(self, name, template_name):
        self.name = '{} section {}'.format(name, next(Test.ids))
        self.template_name = template_name
        template_head = os.path.join(tests_dir,
                                     '{}.head.c'.format(self.template_name))
        with open(template_head, 'r') as f:
            self.source = f.read() + os.linesep
        self.expected_error = []

    def add_source(self, line):
        self.source += line + os.linesep

    def finish_source(self):
        template_tail = os.path.join(tests_dir,
                                     '{}.tail.c'.format(self.template_name))
        with open(template_tail, 'r') as f:
            self.source += f.read()

    def add_error(self, line):
        self.expected_error.append(line)

    def write(self, fp):
        fp.write(self.source)

    def nonmatching_lines(self, output_lines):
        return [line for line in self.expected_error
                if all(map(lambda l: line not in l, output_lines))]


def log_tap_multiline_text(lines):
    print('#     ' + (os.linesep + '#    ').join(lines))


current_test = None
tests = []

# Extract the template name.
template_comment = next(args.infile)
match = re.match(r'\/\*\s*Template:(.*)\*\/', template_comment)
if not match:
    template = 'generic'
else:
    template = match.group(1).strip()
if args.verbose:
    print('# using template {}'.format(template))

# Split the input file up into sections, delimiting on ‘/*’ on a line by itself
for line in args.infile:
    line = line.rstrip(os.linesep)

    if line == '/*':
        if current_test is not None:
            current_test.finish_source()
            tests.append(current_test)
        current_test = Test(args.infile.name, template)
    elif current_test is None:
        continue
    elif line.startswith(' * '):
        if line != ' * No error':
            current_test.add_error(line[3:])

    if current_test is not None:
        current_test.add_source(line)

if current_test is not None:
    current_test.finish_source()
    tests.append(current_test)

tartan_plugin = os.path.join(os.getcwd(), 'clang-plugin', 'libtartan.so')
input_ext = os.path.splitext(args.infile.name)[1]

if input_ext == '.c':
    lang_options = ['-std=c89']
elif input_ext == '.cpp':
    lang_options = ['-xc++']
else:
    lang_options = []

extra_options = []
tartan_test_options = os.environ.get('TARTAN_TEST_OPTIONS', None)
if tartan_test_options is not None:
    extra_options.append(tartan_test_options)

test_env = {
    'TARTAN_PLUGIN': tartan_plugin,
    'TARTAN_OPTIONS': '--quiet'
}
env = dict(os.environ, **test_env)
if args.target_cc:
    env['TARTAN_CC'] = args.target_cc

print('1..{}'.format(len(tests)))
if args.verbose:
    print('# reading input from {}'.format(args.infile.name))
    print('# using tartan from {}'.format(tartan))
    print('# using plugin from {}'.format(tartan_plugin))

for ix, test in enumerate(tests):
    fd, path = tempfile.mkstemp(suffix=input_ext, text=True)
    with os.fdopen(fd, 'wt') as fp:
        test.write(fp)

    # Run the compiler.
    # e.g. Set
    # TARTAN_TEST_OPTIONS="-analyzer-checker=debug.ViewExplodedGraph" to
    # debug the ExplodedGraph
    command = ([tartan, '-cc1', '-analyze', '-Wno-visibility'] + lang_options +
               extra_options + glib_includes + system_includes + [path])
    if args.verbose:
        print('# compiling {}'.format(' '.join(command)))
    result = subprocess.run(command, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE,
                            universal_newlines=True)

    output_lines = result.stderr.splitlines()
    failed = False

    # Compare the errors.
    if test.expected_error:
        # Expecting an error. Check that the expected errors are a
        # subset of the actual errors, to allow for spurious Clang
        # warnings because generated code is hard.
        nonmatching_lines = test.nonmatching_lines(output_lines)
        if nonmatching_lines:
            failed = True
            for line in nonmatching_lines:
                print('# Non-matching line: {}'.format(line))
            print('# Error: expected compiler error was not seen.')
            print('# Expected:')
            log_tap_multiline_text(test.expected_error)
            print('# Actual:')
            log_tap_multiline_text(output_lines)
    else:
        # Expecting no error.
        if result.stderr:
            failed = True
            print('# Error: compiler error when none was expected.')
            log_tap_multiline_text(output_lines)

    if failed:
        # Leave the temporary directory alone on failure.
        if args.verbose:
            print('# not deleting {}'.format(path))
    else:
        os.remove(path)

    print('{}ok {} {}'.format('not ' if failed else '', ix + 1, test.name))
