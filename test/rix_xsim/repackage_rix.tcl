set root [file normalize [lindex $argv 0]]
set component [file join $root ip_repo zz9k_rasterix_1.0 core component.xml]
ipx::open_core $component
set core [ipx::current_core]
ipx::update_checksums $core
ipx::check_integrity -quiet $core
ipx::save_core $core
close_project
exit
