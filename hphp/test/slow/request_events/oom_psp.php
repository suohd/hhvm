<?php

ini_set('memory_limit', 3);
function foo() {
  $x = array();
  while (true) {
    $x[] = str_repeat("asd", 100);
  }
}

function go() {
  echo "hi\n";
  foo();
}

register_postsend_function('go');

foo();
