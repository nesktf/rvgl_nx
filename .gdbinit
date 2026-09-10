define my_bt
  set $frame = $fp
  set $prev_frame = 0
  while $frame != 0 && $prev_frame != $frame
      set $prev_frame = $frame
      p/x ((unsigned long long *)$frame)[1]
      set $frame = ((unsigned long long *)$frame)[0]
  end
end

target extended-remote 192.168.0.75:22225
monitor wait application
