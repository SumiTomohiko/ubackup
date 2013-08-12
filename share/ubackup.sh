
method="$1"
shift
case "${method}" in
"local")
    cmd=""
    ;;
"ssh")
    cmd="ssh $1"
    shift
    ;;
*)
    echo "unsupported method: ${method}"
    exit 1
esac

srcdirs=""
while [ $# -gt 1 ]; do
    srcdirs="${srcdirs} $1"
    shift
done

destdir="$1"

# vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
