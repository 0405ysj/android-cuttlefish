#!/usr/bin/python3
#
# Copyright (C) 2018 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0(the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
"""Parse changelog and get either latest version or stable version.

"""

import os.path
import re
import sys

def main():
    stable_version = False
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        # Exit non-zero if input parameter format is wrong
        print("length is wrong")
        sys.exit(1)
    elif len(sys.argv) == 3:
        # 2nd input parameter is optional
        # latest or stable, default is latest
        if sys.argv[2].lower() == 'stable':
            stable_version = True

    changelog = sys.argv[1]
    if not os.path.isfile(changelog):
        print("changelog {} does not exists".format(changelog))
        sys.exit(1)

    file1 = open(changelog, 'r')
    Lines = file1.readlines()

    stable_version_list = []
    version_list = []

    for line in Lines:
        token_group = re.search(r'.*cuttlefish-common \((.*)\) ([a-zA-Z]+); .*', line)
        if token_group:
            if token_group[2].lower() == 'stable':
                stable_version_list.append(token_group[1])
            else:
                version_list.append(token_group[1])

    if stable_version:
        print(stable_version_list[0])
    else:
        print(version_list[0])


if __name__ == '__main__':
    main()