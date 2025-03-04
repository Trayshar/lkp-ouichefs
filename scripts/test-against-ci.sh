#!/bin/bash

currentbranch=$(git branch --show-current)
git branch squash-for-ci
git checkout squash-for-ci
git reset 27ffab6138569b227cfd3257dabbc85ba28f3a15
git add .
git commit -m "project: squashed for CI"
git format-patch HEAD~1
git send-email --to lkp-maintainers@os.rwth-aachen.de 0001-project-squashed-for-CI.patch
rm 0001-project-squashed-for-CI.patch
git checkout "$currentbranch"
git branch -D squash-for-ci
