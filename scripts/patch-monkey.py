import sys

info = 'DeviceInfo'
parent = 'TYPE_ISA_DEVICE'
func = 'isa_qdev_register'

lines = sys.stdin.read().split('\n')

output = ''
def emit(line):
    global output
    output += '%s\n' % line

i = 0
while i < len(lines):
    line = lines[i]
    i += 1
    begin = i

    if line.startswith('static %s ' % info):
        if not line.endswith('info = {'):
            raise Exception('Cannot process this form "%s"' % line)

        name = line.split()[2][:-5]

        mapped_items = {}
        items = []
        processed_lines = []
        while i < len(lines) and lines[i] != '};':
            line = lines[i]
            i += 1
            processed_lines.append(line)

            if line.strip() == '' or line.strip().startswith('/*'):
                continue

            try:
                key, value = map(lambda x: x.strip(), line.split('=', 1))
                if value.endswith(','):
                    value = value[:-1]
            except:
                sys.stdout.write('\n'.join(processed_lines))
                raise

            if key == '.props' and value.startswith('('):
                properties = []
                while i < len(lines) and lines[i].strip() not in ['},', '}']:
                    line = lines[i]
                    i += 1

                    line = line[8:]
                    if line.endswith(','):
                        line = line[:-1]

                    properties.append(line)

                if i == len(lines):
                    raise Exception('Cannot find end of properties')

                i += 1
                value = properties

            mapped_items[key] = value
            items.append((key, value))

        if i == len(lines):
            raise Exception('Cannot find end of type info')

        i += 1

        props = filter(lambda (x,y): x == '.props', items)
        if len(props) and type(props[0][1]) == list:
            emit('static Property %s_properties[] = {' % name)
            for line in props[0][1]:
                emit('    %s,' % line)
            emit('};')
            emit('')

        if mapped_items.has_key('.class_init'):
            class_init = mapped_items['.class_init']
            for j in range(i):
                if lines[i - j].startswith('static void %s(' % class_init):
                    k = 1
                    emit(lines[i - j])
                    emit(lines[i - j + k])
                    emit('    DeviceClass *k = DEVICE_CLASS(klass);')
                    k += 1
                    while lines[i - j + k] != '}':
                        emit(lines[i - j + k])
                        k += 1
                    lines = lines[0:i - j] + lines[i - j + k + 1:]
                    begin -= k + 3
                    break
        else:
            class_init = '%s_class_init' % name
            emit('static void %s(ObjectClass *klass, void *data)' % class_init)
            emit('{')
            emit('    DeviceClass *k = DEVICE_CLASS(klass);')
            emit('')

        for item in ['fw_name', 'alias', 'desc', 'props', 'no_user', 'reset', 'vmsd']:
            key = '.%s' % item
            if mapped_items.has_key(key):
                if item == 'props' and type(value) == list:
                    emit('    k->props = &%s_properties;' % name)
                else:
                    emit('    k->%s = %s;' % (item, mapped_items[key]))
        emit('}')
        emit('')

        emit('static TypeInfo %s_type_info = {' % name)
        emit('    .name = %s,' % mapped_items['.name'])
        emit('    .parent = %s,' % parent)
        emit('    .instance_size = %s,' % mapped_items['.size'])

        emit('    .class_init = %s,' % class_init)
        emit('};')

        print '\n'.join(lines[0:begin])
        sys.stdout.write(output)
        output = ''
        def fixup(line):
            if line.strip().startswith('isa_qdev_register('):
                return '    type_register_static(&%s_type_info);' % name
            return line
        print '\n'.join(map(fixup, lines[i - k - 1:-1]))
        break

    elif i < len(lines):
        pass
