import sys

def parse_device_info(lines):
    info = {}

    i = 1
    while i < len(lines):
        if lines[i].strip() == '};':
            break

        line = lines[i].strip()
        if line.startswith('//') or line.startswith('/*'):
            i += 1
            continue

        key, value = line.split('=', 1)
        key = key.strip()
        value = value.strip()

        if key == '.props' and value.startswith('('):
            values = []
            i += 1
            while i < len(lines):
                line = lines[i].strip()
                if line.startswith('//') or line.startswith('/*'):
                    i += 1
                    continue
                if line in ['}', '},']:
                    break
                values.append(lines[i])
                i += 1
            value = values
        else:
            if value.find('/*') != -1 or value.find('//') != -1:
                raise Exception('embedded comment')
            if value.endswith(','):
                value = value[:-1]

        info[key[1:]] = value

        i += 1
    return info

def process(lines):
    i = 0
    while i < len(lines):
        if lines[i].startswith('static DeviceInfo '):
            start = i

            i += 1
            while lines[i].strip() != '};':
                i += 1
            end = i + 1
            break
        i += 1

    if i == len(lines):
        return None

    line = lines[start]

    info = line.split()[2]

    while i < len(lines):
        if lines[i].strip().startswith('qdev_register_subclass'):
            line = lines[i].strip()
            _, line = line.split('(', 1)

            target_info, typename = line.split(',', 1)
            typename, _ = typename.split(')', 1)

            target_info = target_info.strip()
            typename = typename.strip()

            if target_info.startswith('&'):
                target_info = target_info[1:].strip()

            if target_info == info:
                info_at = i
                break

        i += 1

    devinfo = parse_device_info(lines[start:][:(end-start-1)])

    needs_dc = False
    for field in ['fw_name', 'alias', 'desc', 'no_user', 'reset', 'vmsd', 'props']:
        if devinfo.has_key(field):
            needs_dc = True
            break

    if devinfo.has_key('class_init'):
        i = 0
        while i < start:
            line = lines[i].strip()
            if line.startswith('static void %s(' % devinfo['class_init']):
                break
            i += 1
        if i == start:
            raise Exception("could not find class_init")

        class_init_begin = i

        while i < start:
            if line == '}':
                break
            i += 1

        class_init_end = i

        print '\n'.join(lines[0:class_init_begin])
        if devinfo.has_key('props') and type(devinfo['props']) == list:
            print 'static Property %s[] = {' % info.replace('info', 'properties')
            for prop in devinfo['props']:
                print prop[4:]
            print '};'
            print

        print '\n'.join(lines[class_init_begin:][0:2])
        if needs_dc:
            print '    DeviceClass *dc = DEVICE_CLASS(klass);'
        print '\n'.join(lines[class_init_begin+2:][0:class_init_end-class_init_begin-4])
        for field in ['fw_name', 'alias', 'desc', 'no_user', 'reset',
                      'vmsd']:
            if devinfo.has_key(field):
                print '    dc->%s = %s;' % (field, devinfo[field])
        if devinfo.has_key('props'):
            if type(devinfo['props']) == list:
                print '    dc->props = %s;' % info.replace('info', 'properties')
            else:
                print '    dc->props = %s;' % devinfo['props']
        print '}'
        print '\n'.join(lines[class_init_end:][0:start-class_init_end])
    else:
        print '\n'.join(lines[0:start])
        if devinfo.has_key('props') and type(devinfo['props']) == list:
            print 'static Property %s[] = {' % info.replace('info', 'properties')
            for prop in devinfo['props']:
                print prop[4:]
            print '};'
            print
        if needs_dc:
            print 'static void %s(ObjectClass *klass, void *data)' % info.replace('info', 'class_init')
            print '{'
            print '    DeviceClass *dc = DEVICE_CLASS(klass);'
            for field in ['fw_name', 'alias', 'desc', 'no_user', 'reset',
                          'vmsd']:
                if devinfo.has_key(field):
                    print '    dc->%s = %s;' % (field, devinfo[field])
            if devinfo.has_key('props'):
                if type(devinfo['props']) == list:
                    print '    dc->props = %s;' % info.replace('info', 'properties')
                else:
                    print '    dc->props = %s;' % devinfo['props']
            print '}'

    print 'static TypeInfo %s = {' % info
    print '    .name          = %s,' % devinfo['name']
    print '    .parent        = %s,' % typename
    print '    .instance_size = %s,' % devinfo['size']
    print '    .class_init    = %s,' % devinfo['class_init']

    print '};'

    print '\n'.join(lines[end:][0:info_at-end])

    print '    type_register_static(&%s);' % info
    if devinfo.has_key('alias'):
        print '    type_register_static_alias(&%s, %s);' % (info, devinfo['alias'])

    sys.stdout.write('\n'.join(lines[info_at+1:]))

    return lines
        
lines = sys.stdin.read().split('\n')
process(lines)
